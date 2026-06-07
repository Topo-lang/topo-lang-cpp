// CppSymbolExtractor — Extract symbols from C++ source files.
//
// Strategy: regex-based line scanning with namespace/class scope tracking.
// Extracts function definitions, class/struct declarations, and tracks
// public:/private:/protected: sections for host visibility.

#include "CppSymbolExtractor.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <stack>
#include <string>
#include <vector>

namespace topo::check {

namespace {

/// True if `R` at index `i` opens a raw string literal (`R"delim(`), as opposed
/// to a normal identifier character that merely happens to precede a `"` (e.g.
/// the `R` ending `"eventR"`). A raw-string prefix sits at a token boundary:
/// the preceding char is not part of an identifier, except for the encoding
/// prefixes `L`, `u`, `U`, `u8`. We only reach this with `line[i] == 'R'`.
bool isRawStringStart(const std::string& line, size_t i) {
    if (i + 1 >= line.size() || line[i + 1] != '"') return false;
    if (i == 0) return true;
    char prev = line[i - 1];
    // Encoding prefixes that may legitimately precede the `R`.
    if (prev == 'L' || prev == 'u' || prev == 'U') {
        // Prefix is L / u / U; it must itself sit at a token boundary.
        return i < 2 || !(std::isalnum(static_cast<unsigned char>(line[i - 2])) || line[i - 2] == '_');
    }
    if (prev == '8' && i >= 2 && line[i - 2] == 'u') {
        // `u8R"` prefix: the char before `R` is `8`, preceded by `u`.
        return i < 3 || !(std::isalnum(static_cast<unsigned char>(line[i - 3])) || line[i - 3] == '_');
    }
    // Any other identifier char before `R` means this is not a raw-string
    // prefix (the `R` belongs to a longer token / string).
    return !(std::isalnum(static_cast<unsigned char>(prev)) || prev == '_');
}

/// Replace string/char/raw-string literal contents with spaces and strip
/// comments on a single line, carrying block-comment and raw-string state
/// across lines. Masked regions become spaces (length-preserving) so braces,
/// identifiers, and punctuation outside literals/comments still line up; a
/// trailing line comment is truncated. A mismatched `{`/`}`/quote/`R"` inside
/// a literal or comment can no longer desync the scanner.
std::string maskLine(const std::string& line, bool& inBlockComment, bool& inRawString, std::string& rawDelim) {
    std::string out(line.size(), ' ');
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inBlockComment) {
            if (c == '*' && i + 1 < line.size() && line[i + 1] == '/') {
                inBlockComment = false;
                ++i;
            }
            continue;
        }
        if (inRawString) {
            std::string closer = ")" + rawDelim + "\"";
            if (line.compare(i, closer.size(), closer) == 0) {
                inRawString = false;
                rawDelim.clear();
                i += closer.size() - 1;
            }
            continue;
        }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            break; // rest of line is a line comment — leave it blank
        }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            inBlockComment = true;
            ++i;
            continue;
        }
        if (c == 'R' && isRawStringStart(line, i)) {
            auto parenPos = line.find('(', i + 2);
            if (parenPos != std::string::npos) {
                std::string delim = line.substr(i + 2, parenPos - (i + 2));
                std::string closer = ")" + delim + "\"";
                auto closePos = line.find(closer, parenPos + 1);
                if (closePos != std::string::npos) {
                    i = closePos + closer.size() - 1; // same-line raw string
                    continue;
                }
                inRawString = true;
                rawDelim = delim;
                continue;
            }
        }
        if (c == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                ++i;
            }
            continue; // closing quote (if present) stays blank
        }
        if (c == '\'') {
            ++i;
            while (i < line.size() && line[i] != '\'') {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                ++i;
            }
            continue;
        }
        out[i] = c;
    }
    return out;
}

// C++ keywords that look like function definitions but aren't
static const std::vector<std::string> cppKeywords = {
    "if",        "for",       "while",    "switch",        "return",    "class",    "struct",  "enum",     "typedef",
    "using",     "template",  "catch",    "else",          "do",        "try",      "throw",   "delete",   "new",
    "sizeof",    "alignof",   "decltype", "static_assert", "namespace", "extern",   "virtual", "explicit", "inline",
    "constexpr", "consteval", "co_await", "co_return",     "co_yield",  "requires", "concept", "noexcept"};

bool isCppKeyword(const std::string& name) {
    for (const auto& kw : cppKeywords) {
        if (name == kw) return true;
    }
    return false;
}

std::string buildQualified(const std::stack<std::string>& scopeStack) {
    std::vector<std::string> parts;
    std::stack<std::string> tmp = scopeStack;
    while (!tmp.empty()) {
        parts.push_back(tmp.top());
        tmp.pop();
    }
    std::string result;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!result.empty()) result += "::";
        result += *it;
    }
    return result;
}

/// Detect visibility from C++ access specifier in current line context.
std::optional<Visibility> detectAccessSpecifier(const std::string& line) {
    // Match "public:", "private:", "protected:" (possibly with whitespace)
    std::regex accessRegex(R"(^\s*(public|private|protected)\s*:)");
    std::smatch m;
    if (std::regex_search(line, m, accessRegex)) {
        std::string spec = m[1].str();
        if (spec == "public") return Visibility::Public;
        if (spec == "private") return Visibility::Private;
        if (spec == "protected") return Visibility::Protected;
    }
    return std::nullopt;
}

/// Extract return type from the portion of line before the function name.
/// Strips keywords like static, virtual, inline, constexpr, explicit, friend.
static std::string extractReturnType(const std::string& line, const std::string& funcName) {
    auto namePos = line.find(funcName + "(");
    if (namePos == std::string::npos) namePos = line.find(funcName + " (");
    if (namePos == std::string::npos) return "";

    std::string before = line.substr(0, namePos);

    // Remove leading whitespace
    auto start = before.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    before = before.substr(start);

    // Strip known C++ keywords that aren't part of the return type
    static const std::vector<std::string> keywords = {
        "static", "virtual", "inline", "constexpr", "consteval", "explicit", "friend", "extern", "override", "final"};
    for (const auto& kw : keywords) {
        // Remove keyword followed by whitespace, repeatedly
        while (true) {
            auto pos = before.find(kw);
            if (pos == std::string::npos) break;
            // Ensure it's a whole word
            auto end = pos + kw.size();
            if (end < before.size() && (std::isalnum(before[end]) || before[end] == '_')) break;
            if (pos > 0 && (std::isalnum(before[pos - 1]) || before[pos - 1] == '_')) break;
            before.erase(pos, end - pos);
        }
    }

    // Trim whitespace
    auto first = before.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    auto last = before.find_last_not_of(" \t");
    return before.substr(first, last - first + 1);
}

/// Extract parameter types from the text between ( and ).
/// For each parameter "const int& x", extracts "const int&" (everything before last token).
/// For parameterless functions "()" returns empty vector.
/// For "(void)" returns empty vector.
static std::vector<std::string> extractParamTypes(const std::string& line) {
    auto openParen = line.find('(');
    auto closeParen = line.rfind(')');
    if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen) return {};

    std::string params = line.substr(openParen + 1, closeParen - openParen - 1);

    // Trim
    auto start = params.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    params = params.substr(start);
    auto end = params.find_last_not_of(" \t");
    params = params.substr(0, end + 1);

    if (params.empty() || params == "void") return {};

    // Split by comma, respecting angle brackets and parens
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (char c : params) {
        if (c == '<' || c == '(')
            ++depth;
        else if (c == '>' || c == ')')
            --depth;
        else if (c == ',' && depth == 0) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty()) parts.push_back(current);

    std::vector<std::string> types;
    for (auto& p : parts) {
        // Trim each param
        auto s = p.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        auto e = p.find_last_not_of(" \t");
        p = p.substr(s, e - s + 1);

        // For "const int& x", the type is everything except the last token
        // But for "int" alone (no name), it's just the type
        // Heuristic: if last char is alphanumeric and there's a space/& /* before it,
        // the last token is the name
        auto lastSpace = p.find_last_of(" \t&*");
        if (lastSpace != std::string::npos && lastSpace < p.size() - 1) {
            // Check if what's after lastSpace looks like an identifier
            auto nameStart = lastSpace + 1;
            if (std::isalpha(p[nameStart]) || p[nameStart] == '_') {
                // It's "type name" — extract just the type part
                std::string type = p.substr(0, nameStart);
                auto te = type.find_last_not_of(" \t");
                if (te != std::string::npos) type = type.substr(0, te + 1);
                types.push_back(type);
                continue;
            }
        }
        // No clear name separation — treat entire thing as type
        types.push_back(p);
    }

    return types;
}

} // anonymous namespace

std::vector<HostSymbol> CppSymbolExtractor::extractSymbols(const std::string& filePath) {
    std::vector<HostSymbol> result;

    std::ifstream file(filePath);
    if (!file.is_open()) return result;

    // Scope tracking
    std::stack<std::string> nsStack; // namespace scopes
    std::stack<int> nsDepths;
    std::stack<std::string> classStack; // class/struct scopes
    std::stack<int> classDepths;

    // Current visibility in class context (default Private for C++ classes)
    std::optional<Visibility> currentClassVis;
    bool inClass = false;

    // Regex patterns
    std::regex funcDefRegex(R"(^\s*(?:\w[\w:*&<> ]*?)\s+(\w+)\s*\()");
    std::regex nsRegex(R"(^\s*namespace\s+([\w:]+)\s*\{)");
    std::regex classRegex(
        R"(^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+(\w+)\s*(?:final\s*)?(?::\s*(?:public|private|protected)\s+[\w:<> ]+\s*)?\{)");
    std::regex constructorRegex(R"(^\s*(?:explicit\s+)?(\w+)\s*\()");
    std::regex destructorRegex(R"(^\s*(?:virtual\s+)?~(\w+)\s*\()");

    std::string line;
    int lineNum = 0;
    int braceDepth = 0;

    // Literal/comment masking state, carried across lines.
    bool inBlockComment = false;
    bool inRawString = false;
    std::string rawDelim;

    while (std::getline(file, line)) {
        ++lineNum;

        // Mask string/char/raw-string contents and strip comments before any
        // scanning, so a `{`/`}` inside a literal or comment cannot pop a
        // namespace/class scope early. All scanning below runs on `line`
        // (the masked text).
        line = maskLine(line, inBlockComment, inRawString, rawDelim);

        // Track braces
        for (char c : line) {
            if (c == '{') {
                ++braceDepth;
            } else if (c == '}') {
                --braceDepth;
                if (braceDepth < 0) braceDepth = 0; // clamp: never go negative
                if (!nsDepths.empty() && braceDepth == nsDepths.top()) {
                    nsStack.pop();
                    nsDepths.pop();
                }
                if (!classDepths.empty() && braceDepth == classDepths.top()) {
                    classStack.pop();
                    classDepths.pop();
                    inClass = !classStack.empty();
                    currentClassVis = inClass ? std::optional<Visibility>(Visibility::Private) : std::nullopt;
                }
            }
        }

        // Check for access specifier (public:/private:/protected:)
        auto accessSpec = detectAccessSpecifier(line);
        if (accessSpec.has_value() && inClass) {
            currentClassVis = *accessSpec;
            continue;
        }

        // Check for namespace
        std::smatch nsMatch;
        if (std::regex_search(line, nsMatch, nsRegex)) {
            // Skip single-line namespace blocks (e.g., forward declarations
            // "namespace engine { void init(); }") — braces already balanced
            // during tracking above, so pushing would leave a stale scope.
            int opens = 0, closes = 0;
            for (char c : line) {
                if (c == '{')
                    ++opens;
                else if (c == '}')
                    ++closes;
            }
            if (opens != closes) {
                nsStack.push(nsMatch[1].str());
                nsDepths.push(braceDepth - 1);
            }
            continue;
        }

        // Check for class/struct declaration
        std::smatch classMatch;
        if (std::regex_search(line, classMatch, classRegex)) {
            std::string className = classMatch[1].str();

            // Record the class symbol
            HostSymbol cls;
            std::string prefix = buildQualified(nsStack);
            if (!classStack.empty()) {
                std::string classPrefix = buildQualified(classStack);
                if (!prefix.empty()) prefix += "::";
                prefix += classPrefix;
            }
            cls.qualifiedName = prefix.empty() ? className : prefix + "::" + className;
            cls.simpleName = className;
            cls.kind = (line.find("struct") != std::string::npos) ? HostSymbolKind::Struct : HostSymbolKind::Class;
            cls.file = filePath;
            cls.line = lineNum;
            result.push_back(std::move(cls));

            classStack.push(className);
            classDepths.push(braceDepth - 1);
            inClass = true;
            // struct defaults to public, class to private
            currentClassVis = (line.find("struct") != std::string::npos) ? Visibility::Public : Visibility::Private;
            continue;
        }

        // Check for destructor
        std::smatch dtorMatch;
        if (inClass && std::regex_search(line, dtorMatch, destructorRegex)) {
            std::string dtorName = dtorMatch[1].str();
            if (!classStack.empty() && dtorName == classStack.top()) {
                HostSymbol sym;
                std::string prefix = buildQualified(nsStack);
                std::string classPrefix = buildQualified(classStack);
                if (!prefix.empty()) prefix += "::";
                prefix += classPrefix;
                sym.qualifiedName = prefix + "::~" + dtorName;
                sym.simpleName = "~" + dtorName;
                sym.kind = HostSymbolKind::Destructor;
                sym.file = filePath;
                sym.line = lineNum;
                sym.enclosingClass = prefix;
                sym.hostVisibility = currentClassVis;
                result.push_back(std::move(sym));
                continue;
            }
        }

        // Check for function/method definition
        std::smatch funcMatch;
        if (std::regex_search(line, funcMatch, funcDefRegex)) {
            std::string funcName = funcMatch[1].str();

            if (isCppKeyword(funcName)) continue;

            // Check if this is a constructor (name == class name)
            bool isConstructor = inClass && !classStack.empty() && funcName == classStack.top();

            HostSymbol sym;
            std::string prefix = buildQualified(nsStack);
            if (!classStack.empty()) {
                std::string classPrefix = buildQualified(classStack);
                if (!prefix.empty()) prefix += "::";
                prefix += classPrefix;
                sym.enclosingClass = prefix;
            }
            sym.qualifiedName = prefix.empty() ? funcName : prefix + "::" + funcName;
            sym.simpleName = funcName;
            sym.file = filePath;
            sym.line = lineNum;

            // Extract return type and parameter types
            if (!isConstructor) {
                sym.returnType = extractReturnType(line, funcName);
            }
            sym.paramTypes = extractParamTypes(line);

            if (isConstructor) {
                sym.kind = HostSymbolKind::Constructor;
            } else if (inClass) {
                // Check for static
                if (line.find("static") != std::string::npos) {
                    sym.kind = HostSymbolKind::StaticMethod;
                    sym.isStatic = true;
                } else {
                    sym.kind = HostSymbolKind::Method;
                }
            } else {
                sym.kind = HostSymbolKind::Function;
            }

            // Check const qualifier
            if (line.find("const") != std::string::npos && line.find("const") > line.find('(')) {
                sym.isConst = true;
            }

            // Host visibility
            if (inClass) {
                sym.hostVisibility = currentClassVis;
            }

            result.push_back(std::move(sym));
        } else if (inClass && !classStack.empty()) {
            // Fallback: try constructorRegex for bare constructors like Foo()
            std::smatch ctorMatch;
            if (std::regex_search(line, ctorMatch, constructorRegex)) {
                std::string ctorName = ctorMatch[1].str();
                if (!isCppKeyword(ctorName) && ctorName == classStack.top()) {
                    HostSymbol sym;
                    std::string prefix = buildQualified(nsStack);
                    if (!classStack.empty()) {
                        std::string classPrefix = buildQualified(classStack);
                        if (!prefix.empty()) prefix += "::";
                        prefix += classPrefix;
                        sym.enclosingClass = prefix;
                    }
                    sym.qualifiedName = prefix.empty() ? ctorName : prefix + "::" + ctorName;
                    sym.simpleName = ctorName;
                    sym.file = filePath;
                    sym.line = lineNum;
                    sym.kind = HostSymbolKind::Constructor;
                    sym.paramTypes = extractParamTypes(line);
                    if (currentClassVis.has_value()) {
                        sym.hostVisibility = currentClassVis.value();
                    }
                    result.push_back(std::move(sym));
                }
            }
        }
    }

    return result;
}

} // namespace topo::check
