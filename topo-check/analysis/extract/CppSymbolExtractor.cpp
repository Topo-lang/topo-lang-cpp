// CppSymbolExtractor — Extract symbols from C++ source files.
//
// Strategy: regex-based line scanning with namespace/class scope tracking.
// Extracts function definitions, class/struct declarations, and tracks
// public:/private:/protected: sections for host visibility.

#include "CppSymbolExtractor.h"

#include <fstream>
#include <regex>
#include <stack>
#include <string>
#include <vector>

namespace topo::check {

namespace {

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

    while (std::getline(file, line)) {
        ++lineNum;

        // Track braces
        for (char c : line) {
            if (c == '{') {
                ++braceDepth;
            } else if (c == '}') {
                --braceDepth;
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
