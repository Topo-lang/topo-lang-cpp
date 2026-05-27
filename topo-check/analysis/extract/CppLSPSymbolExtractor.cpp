// CppLSPSymbolExtractor -- LSP-based C++ symbol extraction via clangd.
//
// Uses semantic tokens to find declaration/definition tokens, then
// resolves each via hover to get qualified names and signatures.

#include "CppLSPSymbolExtractor.h"
#include "CppLSPUtils.h"
#include "ClangdBridge.h"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace topo::check {

CppLSPSymbolExtractor::CppLSPSymbolExtractor(lsp::ClangdBridge& bridge)
    : bridge_(bridge) {}

std::string CppLSPSymbolExtractor::parseReturnType(const std::string& hover) {
    // Hover format: "returnType qualifiedName(params)"
    // Find the opening paren
    auto parenPos = hover.find('(');
    if (parenPos == std::string::npos) return "";

    // Find the function name end (just before paren, skip whitespace)
    size_t nameEnd = parenPos;
    while (nameEnd > 0 && hover[nameEnd - 1] == ' ') --nameEnd;
    if (nameEnd == 0) return "";

    // Find the function name start (scan back through qualified name chars)
    size_t nameStart = nameEnd;
    while (nameStart > 0) {
        char c = hover[nameStart - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '~') {
            --nameStart;
        } else {
            break;
        }
    }

    // Everything before the function name is the return type
    std::string retType = hover.substr(0, nameStart);

    // Trim whitespace and strip common C++ qualifiers that aren't part of the return type
    auto first = retType.find_first_not_of(" \t\n");
    if (first == std::string::npos) return "";
    auto last = retType.find_last_not_of(" \t\n");
    retType = retType.substr(first, last - first + 1);

    // Strip leading qualifiers: static, virtual, inline, constexpr, explicit, friend
    static const char* prefixes[] = {"static ", "virtual ", "inline ", "constexpr ",
                                     "consteval ", "explicit ", "friend "};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const char* pfx : prefixes) {
            size_t len = std::strlen(pfx);
            if (retType.size() >= len && retType.compare(0, len, pfx) == 0) {
                retType = retType.substr(len);
                changed = true;
            }
        }
        // Re-trim
        auto s = retType.find_first_not_of(" \t");
        if (s != std::string::npos) retType = retType.substr(s);
    }

    return retType;
}

std::vector<std::string> CppLSPSymbolExtractor::parseParamTypes(const std::string& hover) {
    auto openParen = hover.find('(');
    auto closeParen = hover.rfind(')');
    if (openParen == std::string::npos || closeParen == std::string::npos ||
        closeParen <= openParen) {
        return {};
    }

    std::string params = hover.substr(openParen + 1, closeParen - openParen - 1);

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
        // Heuristic: if last char is alphanumeric and there's a space/&/* before it,
        // the last token is the parameter name
        auto lastSpace = p.find_last_of(" \t&*");
        if (lastSpace != std::string::npos && lastSpace < p.size() - 1) {
            auto nameStart = lastSpace + 1;
            if (std::isalpha(static_cast<unsigned char>(p[nameStart])) || p[nameStart] == '_') {
                std::string type = p.substr(0, nameStart);
                auto te = type.find_last_not_of(" \t");
                if (te != std::string::npos) type = type.substr(0, te + 1);
                types.push_back(type);
                continue;
            }
        }
        // No clear name separation -- treat entire thing as type
        types.push_back(p);
    }

    return types;
}

std::string CppLSPSymbolExtractor::detectEnclosingClass(const std::string& qualifiedName) {
    // "ns::Class::method" -> "ns::Class"
    auto lastSep = qualifiedName.rfind("::");
    if (lastSep == std::string::npos || lastSep == 0) return "";
    return qualifiedName.substr(0, lastSep);
}

std::vector<HostSymbol> CppLSPSymbolExtractor::extractSymbols(const std::string& filePath) {
    std::vector<HostSymbol> result;

    if (!bridge_.isAvailable()) return result;

    // 1. Open document for clangd analysis
    bridge_.openDocument(filePath);
    struct DocGuard {
        lsp::ClangdBridge& b;
        const std::string& path;
        ~DocGuard() { b.closeDocument(path); }
    } guard{bridge_, filePath};

    // 2. Get semantic tokens
    auto tokens = bridge_.getSemanticTokens(filePath);
    if (tokens.empty()) {
        return result;
    }

    // 3. Filter and process declaration/definition tokens
    for (const auto& token : tokens) {
        // Only interested in declarations and definitions
        if (!hasModifier(token.modifiers, "declaration") &&
            !hasModifier(token.modifiers, "definition")) {
            continue;
        }

        // Map semantic token type to HostSymbolKind
        bool isFunction = (token.type == "function");
        bool isMethod = (token.type == "method");
        bool isClass = (token.type == "class");
        bool isStruct = (token.type == "struct");
        bool isEnum = (token.type == "enum");

        if (!isFunction && !isMethod && !isClass && !isStruct && !isEnum) continue;

        // 4. Resolve via hover to get qualified name and signature
        auto hover = bridge_.getHoverAt(filePath, token.line, token.column);
        if (!hover) continue;

        std::string qualifiedName = extractQualifiedName(*hover);
        if (qualifiedName.empty()) continue;

        HostSymbol sym;
        sym.qualifiedName = qualifiedName;
        sym.file = filePath;
        sym.line = token.line + 1; // semantic tokens are 0-based, HostSymbol is 1-based

        // Extract simple name from qualified name
        auto lastSep = qualifiedName.rfind("::");
        sym.simpleName = (lastSep != std::string::npos)
                             ? qualifiedName.substr(lastSep + 2)
                             : qualifiedName;

        // Determine kind
        if (isClass) {
            sym.kind = HostSymbolKind::Class;
        } else if (isStruct) {
            sym.kind = HostSymbolKind::Struct;
        } else if (isEnum) {
            sym.kind = HostSymbolKind::Enum;
        } else if (isMethod) {
            // Check for static modifier
            if (hasModifier(token.modifiers, "static")) {
                sym.kind = HostSymbolKind::StaticMethod;
                sym.isStatic = true;
            } else {
                sym.kind = HostSymbolKind::Method;
            }

            // Check if this is a constructor or destructor
            std::string enclosing = detectEnclosingClass(qualifiedName);
            if (!enclosing.empty()) {
                sym.enclosingClass = enclosing;

                // Extract the class simple name from enclosing
                auto classSep = enclosing.rfind("::");
                std::string className = (classSep != std::string::npos)
                                            ? enclosing.substr(classSep + 2)
                                            : enclosing;

                if (sym.simpleName == className) {
                    sym.kind = HostSymbolKind::Constructor;
                } else if (sym.simpleName.size() > 1 && sym.simpleName[0] == '~' &&
                           sym.simpleName.substr(1) == className) {
                    sym.kind = HostSymbolKind::Destructor;
                }
            }

            // Parse return type and params from hover (for non-type symbols)
            sym.returnType = parseReturnType(*hover);
            sym.paramTypes = parseParamTypes(*hover);

            // Check const qualifier from hover
            // In clangd hover, const methods show "const" after the closing paren
            auto closeParen = hover->rfind(')');
            if (closeParen != std::string::npos) {
                std::string afterParen = hover->substr(closeParen + 1);
                if (afterParen.find("const") != std::string::npos) {
                    sym.isConst = true;
                }
            }
        } else if (isFunction) {
            sym.kind = HostSymbolKind::Function;
            sym.returnType = parseReturnType(*hover);
            sym.paramTypes = parseParamTypes(*hover);
        }

        result.push_back(std::move(sym));
    }

    return result;
}

} // namespace topo::check
