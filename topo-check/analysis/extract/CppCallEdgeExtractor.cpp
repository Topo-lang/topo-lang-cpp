// CppCallEdgeExtractor — L1 regex extractor for caller→callee edges.
//
// Mirrors CppCallSiteExtractor's scope-tracking state machine (nsStack /
// classStack / braceDepth / inFunction). For each identifier call in a
// function body, emits a CallEdge with the caller qualified by the current
// namespace/class scope and the callee set to the raw token (already scoped
// via `::` when the call was written as `ns::foo(...)`).

#include "CppCallEdgeExtractor.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <stack>
#include <string>
#include <unordered_set>

namespace topo::check {

namespace {

// Build a caller qualified name from the namespace + class scope stacks.
// Mirrors CppCallSiteExtractor::buildQualified.
std::string buildQualified(const std::stack<std::string>& nsStack,
                           const std::stack<std::string>& classStack,
                           const std::string& funcName) {
    std::vector<std::string> parts;
    {
        std::stack<std::string> tmp = nsStack;
        std::vector<std::string> ns;
        while (!tmp.empty()) {
            ns.push_back(tmp.top());
            tmp.pop();
        }
        for (auto it = ns.rbegin(); it != ns.rend(); ++it) parts.push_back(*it);
    }
    {
        std::stack<std::string> tmp = classStack;
        std::vector<std::string> cls;
        while (!tmp.empty()) {
            cls.push_back(tmp.top());
            tmp.pop();
        }
        for (auto it = cls.rbegin(); it != cls.rend(); ++it) parts.push_back(*it);
    }
    parts.push_back(funcName);

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "::";
        result += parts[i];
    }
    return result;
}

/// True if `R` at index `i` opens a raw string literal (`R"delim(`), not a
/// normal identifier char that merely precedes a `"` (e.g. the `R` ending the
/// string `"eventR"`). A raw-string prefix sits at a token boundary; only the
/// encoding prefixes `L`, `u`, `U`, `u8` may legitimately precede the `R`.
bool isRawStringStart(const std::string& line, size_t i) {
    if (i + 1 >= line.size() || line[i + 1] != '"') return false;
    if (i == 0) return true;
    char prev = line[i - 1];
    if (prev == 'L' || prev == 'u' || prev == 'U') {
        // Prefix is L / u / U; it must itself sit at a token boundary.
        return i < 2 || !(std::isalnum(static_cast<unsigned char>(line[i - 2])) || line[i - 2] == '_');
    }
    if (prev == '8' && i >= 2 && line[i - 2] == 'u') {
        // `u8R"` prefix: the char before `R` is `8`, preceded by `u`.
        return i < 3 || !(std::isalnum(static_cast<unsigned char>(line[i - 3])) || line[i - 3] == '_');
    }
    return !(std::isalnum(static_cast<unsigned char>(prev)) || prev == '_');
}

bool isCommentLine(const std::string& line) {
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos) return false;
    return (line.size() > pos + 1 && line[pos] == '/' && line[pos + 1] == '/');
}

bool isPreprocessorLine(const std::string& line) {
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos || line[pos] != '#') return false;
    std::string rest = line.substr(pos + 1);
    size_t kw = rest.find_first_not_of(" \t");
    if (kw == std::string::npos) return false;
    return (rest.compare(kw, 2, "if") == 0 ||
            rest.compare(kw, 5, "ifdef") == 0 ||
            rest.compare(kw, 6, "ifndef") == 0 ||
            rest.compare(kw, 4, "else") == 0 ||
            rest.compare(kw, 4, "elif") == 0 ||
            rest.compare(kw, 5, "endif") == 0 ||
            rest.compare(kw, 6, "define") == 0 ||
            rest.compare(kw, 7, "include") == 0 ||
            rest.compare(kw, 6, "pragma") == 0 ||
            rest.compare(kw, 5, "error") == 0);
}

// C++ keywords and control statements that regex `\w+\s*\(` would also match.
// Skip these so we don't emit "call edges" like (if, ...) or (return, ...).
const std::unordered_set<std::string>& controlKeywords() {
    static const std::unordered_set<std::string> kws = {
        // Control flow
        "if", "for", "while", "switch", "do", "return", "break", "continue",
        "throw", "catch", "try", "else", "goto",
        // Declarations (when followed by `(`, e.g. constructor calls)
        "class", "struct", "union", "enum", "namespace", "typedef", "using",
        "template", "typename", "friend", "static", "extern", "inline",
        "virtual", "explicit", "const", "constexpr", "consteval", "mutable",
        "volatile", "register", "thread_local", "decltype", "auto", "void",
        // Operators/expressions
        "sizeof", "alignof", "alignas", "typeid", "new", "delete",
        "noexcept", "requires", "concept",
        // Casts
        "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast",
        // Literals
        "true", "false", "nullptr",
        // Coroutines / exceptions
        "co_await", "co_return", "co_yield", "throw",
        // Common primitive types that may appear as `int(x)` etc.
        "int", "char", "short", "long", "float", "double", "bool",
        "signed", "unsigned", "size_t", "int8_t", "int16_t", "int32_t",
        "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "wchar_t",
    };
    return kws;
}

bool isControlKeyword(const std::string& name) {
    const auto& kws = controlKeywords();
    return kws.count(name) > 0;
}

} // namespace

std::vector<CallEdge> CppCallEdgeExtractor::extractCallEdges(const std::string& filePath) {
    std::vector<CallEdge> results;
    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    // Scope tracking (same pattern as CppCallSiteExtractor)
    std::stack<std::string> nsStack;
    std::stack<int> nsDepths;
    std::stack<std::string> classStack;
    std::stack<int> classDepths;

    int braceDepth = 0;
    bool inFunction = false;
    int functionDepth = -1;
    std::string currentFunction;

    // Allman brace fix: remember a pending function signature when { is on next line
    bool pendingSignature = false;
    std::string pendingFunctionName;

    // Block comment / raw string state machine
    bool inBlockComment = false;
    bool inRawString = false;
    std::string rawDelimiter;

    // Regex patterns (mirror CppCallSiteExtractor)
    std::regex nsRegex(R"(^\s*namespace\s+([\w:]+)\s*\{)");
    std::regex classRegex(R"(^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+(\w+)[^;]*\{)");
    std::regex funcDefRegex(R"(^\s*[\w:*&<>, ]+\s+(\w+)\s*\([^)]*\)\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?\{?)");

    // Match call targets: optional scope prefix (`ns::` or `::`), then identifier, then `(`.
    // Captures the full callee token (including any `::` scopes) in group 1.
    std::regex callRegex(R"(((?:[\w]+\s*::\s*)*[\w]+)\s*\()");

    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        // Raw string tracking (highest priority)
        if (inRawString) {
            std::string closer = ")" + rawDelimiter + "\"";
            if (line.find(closer) != std::string::npos) {
                inRawString = false;
                rawDelimiter.clear();
            }
            continue;
        }

        // Block comment tracking
        if (inBlockComment) {
            auto closePos = line.find("*/");
            if (closePos != std::string::npos) {
                inBlockComment = false;
            }
            continue;
        }

        // Skip comment-only lines
        if (isCommentLine(line)) continue;

        // Skip preprocessor directives
        if (isPreprocessorLine(line)) continue;

        // Strip inline comments / block comments / raw strings from the scan line
        std::string effectiveLine = line;
        for (size_t i = 0; i < effectiveLine.size(); ++i) {
            char c = effectiveLine[i];
            if (c == '/' && i + 1 < effectiveLine.size() && effectiveLine[i + 1] == '/') {
                effectiveLine = effectiveLine.substr(0, i);
                break;
            }
            if (c == '/' && i + 1 < effectiveLine.size() && effectiveLine[i + 1] == '*') {
                auto closePos = effectiveLine.find("*/", i + 2);
                if (closePos != std::string::npos) {
                    effectiveLine.erase(i, closePos + 2 - i);
                    --i;
                    continue;
                }
                effectiveLine = effectiveLine.substr(0, i);
                inBlockComment = true;
                break;
            }
            // Regular string / char literal — skip its contents so an `R"` or
            // comment marker inside it is never treated as code.
            if (c == '"' || c == '\'') {
                char quote = c;
                ++i;
                while (i < effectiveLine.size() && effectiveLine[i] != quote) {
                    if (effectiveLine[i] == '\\' && i + 1 < effectiveLine.size()) ++i;
                    ++i;
                }
                continue; // closing quote consumed by loop increment
            }
            if (c == 'R' && isRawStringStart(effectiveLine, i)) {
                auto parenPos = effectiveLine.find('(', i + 2);
                if (parenPos != std::string::npos) {
                    std::string delim = effectiveLine.substr(i + 2, parenPos - (i + 2));
                    std::string closer = ")" + delim + "\"";
                    auto closePos = effectiveLine.find(closer, parenPos + 1);
                    if (closePos != std::string::npos) {
                        effectiveLine.erase(i, closePos + closer.size() - i);
                        --i;
                        continue;
                    }
                    effectiveLine = effectiveLine.substr(0, i);
                    inRawString = true;
                    rawDelimiter = delim;
                    break;
                }
            }
        }

        // Track braces on the effective line, skipping characters inside string literals.
        for (size_t i = 0; i < effectiveLine.size(); ++i) {
            char c = effectiveLine[i];
            if (c == '"') {
                ++i;
                while (i < effectiveLine.size() && effectiveLine[i] != '"') {
                    if (effectiveLine[i] == '\\') ++i;
                    ++i;
                }
                continue;
            }
            if (c == '\'') {
                ++i;
                while (i < effectiveLine.size() && effectiveLine[i] != '\'') {
                    if (effectiveLine[i] == '\\') ++i;
                    ++i;
                }
                continue;
            }
            if (c == '{') {
                ++braceDepth;
            } else if (c == '}') {
                --braceDepth;
                if (braceDepth < 0) braceDepth = 0;
                if (!nsDepths.empty() && braceDepth == nsDepths.top()) {
                    nsStack.pop();
                    nsDepths.pop();
                }
                if (!classDepths.empty() && braceDepth == classDepths.top()) {
                    classStack.pop();
                    classDepths.pop();
                }
                if (inFunction && braceDepth == functionDepth) {
                    inFunction = false;
                    functionDepth = -1;
                    currentFunction.clear();
                }
            }
        }

        // Detect namespace declaration
        std::smatch nsMatch;
        if (std::regex_search(effectiveLine, nsMatch, nsRegex)) {
            nsStack.push(nsMatch[1].str());
            nsDepths.push(braceDepth - 1);
            continue;
        }

        // Detect class/struct declaration
        std::smatch classMatch;
        if (!inFunction && std::regex_search(effectiveLine, classMatch, classRegex)) {
            classStack.push(classMatch[1].str());
            classDepths.push(braceDepth - 1);
            continue;
        }

        // Allman brace fix: pending signature resolved on next line with {
        if (pendingSignature && !inFunction && effectiveLine.find('{') != std::string::npos) {
            inFunction = true;
            functionDepth = braceDepth - 1;
            currentFunction = pendingFunctionName;
            pendingSignature = false;
            pendingFunctionName.clear();
        }

        // Detect function definition
        std::smatch funcMatch;
        if (!inFunction && std::regex_search(effectiveLine, funcMatch, funcDefRegex)) {
            std::string fname = funcMatch[1].str();
            if (!isControlKeyword(fname)) {
                bool hasBrace = effectiveLine.find('{') != std::string::npos;
                // A line ending with `;` is a forward declaration, not a definition.
                bool isForwardDecl = false;
                {
                    auto tail = effectiveLine.find_last_not_of(" \t");
                    if (tail != std::string::npos && effectiveLine[tail] == ';') {
                        isForwardDecl = true;
                    }
                }
                if (!isForwardDecl) {
                    if (hasBrace) {
                        inFunction = true;
                        functionDepth = braceDepth - 1;
                        currentFunction = fname;
                    } else {
                        pendingSignature = true;
                        pendingFunctionName = fname;
                    }
                }
            }
        }

        // Inside a function body: scan for call targets.
        if (inFunction && braceDepth > functionDepth) {
            std::string callerName = buildQualified(nsStack, classStack, currentFunction);

            // Mask string/char literals so call-like tokens inside them are ignored.
            std::string scanLine = effectiveLine;
            for (size_t i = 0; i < scanLine.size(); ++i) {
                char c = scanLine[i];
                if (c == '"') {
                    scanLine[i] = ' ';
                    ++i;
                    while (i < scanLine.size() && scanLine[i] != '"') {
                        if (scanLine[i] == '\\') {
                            scanLine[i] = ' ';
                            if (i + 1 < scanLine.size()) {
                                scanLine[i + 1] = ' ';
                                ++i;
                            }
                        } else {
                            scanLine[i] = ' ';
                        }
                        ++i;
                    }
                    if (i < scanLine.size()) scanLine[i] = ' ';
                } else if (c == '\'') {
                    scanLine[i] = ' ';
                    ++i;
                    while (i < scanLine.size() && scanLine[i] != '\'') {
                        if (scanLine[i] == '\\') {
                            scanLine[i] = ' ';
                            if (i + 1 < scanLine.size()) {
                                scanLine[i + 1] = ' ';
                                ++i;
                            }
                        } else {
                            scanLine[i] = ' ';
                        }
                        ++i;
                    }
                    if (i < scanLine.size()) scanLine[i] = ' ';
                }
            }

            // Iterate call-like tokens in the scan line.
            std::string remaining = scanLine;
            size_t absOffset = 0;
            while (true) {
                std::smatch m;
                if (!std::regex_search(remaining, m, callRegex)) break;
                std::string callee = m[1].str();
                size_t matchPos = absOffset + static_cast<size_t>(m.position(1));
                size_t matchLen = m[1].length();

                // Strip whitespace around `::` for a clean callee token.
                std::string normalized;
                normalized.reserve(callee.size());
                for (char ch : callee) {
                    if (ch != ' ' && ch != '\t') normalized += ch;
                }

                // Extract the simple (last) name for keyword filtering.
                std::string simple;
                auto scopeEnd = normalized.rfind("::");
                if (scopeEnd != std::string::npos) {
                    simple = normalized.substr(scopeEnd + 2);
                } else {
                    simple = normalized;
                }

                bool skip = false;
                if (simple.empty() ||
                    (!std::isalpha(static_cast<unsigned char>(simple[0])) && simple[0] != '_')) {
                    skip = true;
                }
                if (!skip && isControlKeyword(simple)) {
                    skip = true;
                }

                // Skip method calls on objects: `obj.foo(...)` / `ptr->foo(...)`.
                if (!skip && matchPos >= 1 && scanLine.size() > matchPos - 1) {
                    char prev = scanLine[matchPos - 1];
                    if (prev == '.') skip = true;
                }
                if (!skip && matchPos >= 2) {
                    char p1 = scanLine[matchPos - 1];
                    char p2 = scanLine[matchPos - 2];
                    if (p1 == '>' && p2 == '-') skip = true;
                }

                // Skip if the match is the function definition itself
                // (e.g., `void foo() {` produces a call-like match for foo).
                // We already branched into "inside function body" based on
                // braceDepth > functionDepth, so by construction the current
                // line is after the opening brace — but on the same line as
                // the definition, the sig may appear. To be safe, skip when
                // the caller name equals the callee simple name AND the
                // definition brace was just opened on this line.
                if (!skip && simple == currentFunction && effectiveLine.find('{') != std::string::npos) {
                    // The definition line itself — the "call" is the sig.
                    skip = true;
                }

                if (!skip) {
                    CallEdge edge;
                    edge.caller = callerName;
                    edge.callee = normalized;
                    edge.file = filePath;
                    edge.line = lineNum;
                    results.push_back(std::move(edge));
                }

                size_t advance = static_cast<size_t>(m.position(1)) + matchLen;
                if (advance == 0) advance = 1;
                remaining = remaining.substr(advance);
                absOffset += advance;
            }
        }
    }

    return results;
}

} // namespace topo::check
