#ifndef TOPO_CHECK_CPPLSPUTILS_H
#define TOPO_CHECK_CPPLSPUTILS_H

#include <cctype>
#include <string>

namespace topo::check {

/// Remove template argument brackets from a qualified name.
/// "std::vector<int>::begin" -> "std::vector::begin"
/// "std::basic_string<char>" -> "std::basic_string"
inline std::string stripTemplateArgs(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    int depth = 0;
    for (char c : name) {
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            if (depth > 0) --depth;
        } else if (depth == 0) {
            result += c;
        }
    }
    return result;
}

/// Scan backwards from `pos` over a qualified name, handling template args.
/// Returns the start position of the name.
inline size_t scanQualifiedNameBackward(const std::string& text, size_t pos) {
    while (pos > 0) {
        char c = text[pos - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '~') {
            --pos;
        } else if (c == '>') {
            // Skip balanced template argument block <...>
            --pos;
            int depth = 1;
            while (pos > 0 && depth > 0) {
                --pos;
                if (text[pos] == '>') ++depth;
                else if (text[pos] == '<') --depth;
            }
        } else {
            break;
        }
    }
    return pos;
}

/// Extract qualified function name from clangd hover markdown.
///
/// Input examples:
///   "int std::abs(int)"                             -> "std::abs"
///   "void ::system(const char *)"                   -> "system"
///   "int myns::MyClass::method()"                   -> "myns::MyClass::method"
///   "iterator std::vector<int>::begin()"             -> "std::vector::begin"
///   "class engine::Renderer"                         -> "engine::Renderer"
///   "class std::basic_string<char>"                  -> "std::basic_string"
///
/// For types (class/struct/enum) without parens, extracts the last
/// qualified name segment from the hover text.
/// Template arguments in qualified names are stripped from the result.
inline std::string extractQualifiedName(const std::string& hover) {
    // Try function-style first: find opening paren of parameter list
    auto parenPos = hover.find('(');
    if (parenPos != std::string::npos) {
        // Work backwards from the paren to find the function name
        // Skip whitespace before paren
        size_t end = parenPos;
        while (end > 0 && hover[end - 1] == ' ') --end;
        if (end == 0) return "";

        size_t start = scanQualifiedNameBackward(hover, end);

        std::string name = stripTemplateArgs(hover.substr(start, end - start));
        // Strip leading ::
        if (name.size() >= 2 && name[0] == ':' && name[1] == ':') {
            name = name.substr(2);
        }
        return name;
    }

    // Type-style hover: "class ns::Name", "struct ns::Name", "enum ns::Name"
    // Find the last qualified name in the hover text
    size_t end = hover.size();
    // Trim trailing whitespace
    while (end > 0 && std::isspace(static_cast<unsigned char>(hover[end - 1]))) --end;
    if (end == 0) return "";

    size_t nameStart = scanQualifiedNameBackward(hover, end);

    if (nameStart == end) return "";

    std::string name = stripTemplateArgs(hover.substr(nameStart, end - nameStart));
    // Strip leading ::
    if (name.size() >= 2 && name[0] == ':' && name[1] == ':') {
        name = name.substr(2);
    }
    return name;
}

/// Extract the header name from a clangd hover "provided by <header>" line.
/// Returns empty string if no "provided by" line is found.
/// Example: "function sort\n\nprovided by <algorithm>\n..." -> "algorithm"
inline std::string extractProvidedByHeader(const std::string& hover) {
    const std::string marker = "provided by <";
    auto pos = hover.find(marker);
    if (pos == std::string::npos) return "";
    auto start = pos + marker.size();
    auto end = hover.find('>', start);
    if (end == std::string::npos) return "";
    return hover.substr(start, end - start);
}

/// Determine whether a semantic token modifier string contains the given modifier.
/// Modifier strings from clangd are comma-separated, e.g. "declaration,readonly".
inline bool hasModifier(const std::string& modifiers, const std::string& modifier) {
    return modifiers.find(modifier) != std::string::npos;
}

} // namespace topo::check

#endif // TOPO_CHECK_CPPLSPUTILS_H
