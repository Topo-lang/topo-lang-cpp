// CppSymbolAccessExtractor — L1 regex extractor for global symbol writes.
//
// Strategy:
//   Pass 1: scan the file once to collect file-scope globals (variables at
//           namespace or translation-unit scope, plus `extern` and
//           `thread_local`). Member variables (inside class/struct) and
//           locals (inside function bodies) are filtered by scope tracking.
//   Pass 2: re-scan and emit SymbolAccess{isWrite=true} for writes to known
//           globals inside function bodies. Writes include:
//             - simple assignment `name = ...`
//             - compound assignment `name +=/-=/*= ...`
//             - pre/post increment/decrement `++name` / `name++`
//
// Reads are deferred to a later milestone — the load-bearing signal for
// PurityCheck is writes in parallel stages.

#include "CppSymbolAccessExtractor.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stack>
#include <string>
#include <unordered_set>

namespace topo::check {

namespace {

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

bool isCommentLine(const std::string& line) {
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos) return false;
    return (line.size() > pos + 1 && line[pos] == '/' && line[pos + 1] == '/');
}

bool isPreprocessorLine(const std::string& line) {
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos || line[pos] != '#') return false;
    return true;
}

// Strip inline line comments and same-line block comments. Multi-line comments
// leave a trailing marker via the shared state machine (inBlockComment).
std::string stripInlineComments(const std::string& line, bool& inBlockComment, bool& inRawString,
                                std::string& rawDelim) {
    std::string out;
    out.reserve(line.size());
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
            // Look for the closer )delim"
            std::string closer = ")" + rawDelim + "\"";
            if (line.compare(i, closer.size(), closer) == 0) {
                inRawString = false;
                rawDelim.clear();
                i += closer.size() - 1;
            }
            out += ' ';
            continue;
        }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            break; // rest of line is comment
        }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
            inBlockComment = true;
            ++i;
            continue;
        }
        if (c == 'R' && i + 1 < line.size() && line[i + 1] == '"') {
            auto parenPos = line.find('(', i + 2);
            if (parenPos != std::string::npos) {
                std::string delim = line.substr(i + 2, parenPos - (i + 2));
                std::string closer = ")" + delim + "\"";
                auto closePos = line.find(closer, parenPos + 1);
                if (closePos != std::string::npos) {
                    // same-line raw string — skip it
                    out += ' ';
                    i = closePos + closer.size() - 1;
                    continue;
                }
                inRawString = true;
                rawDelim = delim;
                out += ' ';
                continue;
            }
        }
        out += c;
    }
    return out;
}

// Mask characters inside string / char literals so they don't look like code
// to the regex scanner. Returns a copy with literal contents replaced by
// spaces. Comments are handled by stripInlineComments upstream.
std::string maskStringLiterals(const std::string& line) {
    std::string out = line;
    for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (c == '"') {
            out[i] = ' ';
            ++i;
            while (i < out.size() && out[i] != '"') {
                if (out[i] == '\\' && i + 1 < out.size()) {
                    out[i] = ' ';
                    out[i + 1] = ' ';
                    i += 2;
                    continue;
                }
                out[i] = ' ';
                ++i;
            }
            if (i < out.size()) out[i] = ' ';
        } else if (c == '\'') {
            out[i] = ' ';
            ++i;
            while (i < out.size() && out[i] != '\'') {
                if (out[i] == '\\' && i + 1 < out.size()) {
                    out[i] = ' ';
                    out[i + 1] = ' ';
                    i += 2;
                    continue;
                }
                out[i] = ' ';
                ++i;
            }
            if (i < out.size()) out[i] = ' ';
        }
    }
    return out;
}

// C++ type keywords + control keywords that must NOT be mistaken for a
// variable name when scanning for globals.
const std::unordered_set<std::string>& typeKeywords() {
    static const std::unordered_set<std::string> kws = {
        "void", "bool", "char", "short", "int", "long", "float", "double",
        "signed", "unsigned", "auto", "const", "volatile", "mutable",
        "static", "extern", "thread_local", "register", "inline",
        "constexpr", "consteval", "virtual", "explicit", "friend",
        "class", "struct", "union", "enum", "typedef", "using", "namespace",
        "template", "typename", "decltype", "if", "else", "for", "while",
        "do", "switch", "case", "default", "break", "continue", "return",
        "throw", "try", "catch", "new", "delete", "sizeof", "alignof",
        "alignas", "nullptr", "true", "false", "this", "operator",
        "public", "private", "protected", "final", "override",
        "int8_t", "int16_t", "int32_t", "int64_t",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "size_t", "intptr_t", "uintptr_t", "ptrdiff_t", "wchar_t",
        "char16_t", "char32_t",
    };
    return kws;
}

bool isTypeKeyword(const std::string& name) {
    const auto& kws = typeKeywords();
    return kws.count(name) > 0;
}

// Pass 1: scan the file and collect file-scope globals.
// We accept names at namespace/global scope (braceDepth == 0 when outside
// any class) where the statement has the shape of a variable declaration.
std::unordered_set<std::string> collectGlobals(const std::string& filePath) {
    std::unordered_set<std::string> globals;
    std::ifstream file(filePath);
    if (!file.is_open()) return globals;

    std::stack<std::string> nsStack;
    std::stack<int> nsDepths;
    std::stack<std::string> classStack;
    std::stack<int> classDepths;

    int braceDepth = 0;
    bool inFunction = false;
    int functionDepth = -1;

    bool inBlockComment = false;
    bool inRawString = false;
    std::string rawDelim;

    std::regex nsRegex(R"(^\s*namespace\s+([\w:]+)\s*\{)");
    std::regex classRegex(R"(^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+(\w+)[^;]*\{)");
    std::regex funcDefRegex(R"(^\s*[\w:*&<>, ]+\s+(\w+)\s*\([^)]*\)\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?\{?)");

    // Globals are declared with shapes like:
    //   static int counter;
    //   static int counter = 0;
    //   extern int gValue;
    //   thread_local int tlsVar;
    //   int someGlobal;
    //   int someGlobal = 42;
    //   namespace ns { int x; }
    std::regex declRegex(R"(^\s*(?:(?:static|extern|thread_local|const|constexpr|inline|volatile|mutable)\s+)*[\w:*&<>, ]+\s+([\w]+)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;)");

    std::string line;

    while (std::getline(file, line)) {
        // Track comments + raw strings.
        std::string stripped = stripInlineComments(line, inBlockComment, inRawString, rawDelim);
        if (stripped.empty() && !inBlockComment && !inRawString) continue;
        if (isCommentLine(stripped)) continue;
        if (isPreprocessorLine(stripped)) continue;

        std::string masked = maskStringLiterals(stripped);

        // Track braces first.
        for (size_t i = 0; i < masked.size(); ++i) {
            char c = masked[i];
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
                }
            }
        }

        // Detect namespace
        std::smatch nsMatch;
        if (std::regex_search(masked, nsMatch, nsRegex)) {
            nsStack.push(nsMatch[1].str());
            nsDepths.push(braceDepth - 1);
            continue;
        }

        // Detect class/struct
        std::smatch classMatch;
        if (!inFunction && std::regex_search(masked, classMatch, classRegex)) {
            classStack.push(classMatch[1].str());
            classDepths.push(braceDepth - 1);
            continue;
        }

        // Detect function definition — opens a function scope that disables
        // global detection for nested lines.
        std::smatch funcMatch;
        if (!inFunction && std::regex_search(masked, funcMatch, funcDefRegex)) {
            std::string fname = funcMatch[1].str();
            if (!isTypeKeyword(fname)) {
                if (masked.find('{') != std::string::npos) {
                    inFunction = true;
                    functionDepth = braceDepth - 1;
                }
                continue;
            }
        }

        // Only consider declarations at namespace or TU scope (not inside
        // classes or functions).
        if (inFunction) continue;
        if (!classStack.empty()) continue;

        // Match variable declaration at global / namespace scope.
        std::smatch declMatch;
        if (std::regex_search(masked, declMatch, declRegex)) {
            std::string name = declMatch[1].str();
            if (name.empty()) continue;
            if (isTypeKeyword(name)) continue;
            // Skip things that look like function declarations (shouldn't
            // happen — funcDefRegex filters those — but be defensive).
            if (masked.find('(') != std::string::npos) {
                // Check if '(' appears before `name` — if so it's probably
                // a function decl we missed.
                size_t namePos = masked.find(name);
                size_t parenPos = masked.find('(');
                if (parenPos != std::string::npos && namePos != std::string::npos && parenPos < namePos) {
                    continue;
                }
                if (parenPos != std::string::npos && parenPos > namePos) {
                    // `int foo(...);` is a function decl — skip.
                    continue;
                }
            }
            globals.insert(name);
        }
    }

    return globals;
}

} // namespace

std::vector<SymbolAccess> CppSymbolAccessExtractor::extractSymbolAccesses(const std::string& filePath) {
    std::vector<SymbolAccess> results;

    // Pass 1: collect globals.
    auto globals = collectGlobals(filePath);
    if (globals.empty()) return results;

    // Pass 2: scan function bodies for writes.
    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    std::stack<std::string> nsStack;
    std::stack<int> nsDepths;
    std::stack<std::string> classStack;
    std::stack<int> classDepths;

    int braceDepth = 0;
    bool inFunction = false;
    int functionDepth = -1;
    std::string currentFunction;

    bool pendingSignature = false;
    std::string pendingFunctionName;

    bool inBlockComment = false;
    bool inRawString = false;
    std::string rawDelim;

    std::regex nsRegex(R"(^\s*namespace\s+([\w:]+)\s*\{)");
    std::regex classRegex(R"(^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+(\w+)[^;]*\{)");
    std::regex funcDefRegex(R"(^\s*[\w:*&<>, ]+\s+(\w+)\s*\([^)]*\)\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?\{?)");

    // Write candidates: for each known global name, match:
    //   name = ...
    //   name += / -= / *= / /= / %= / &= / |= / ^= / <<= / >>= ...
    //   ++name / name++ / --name / name--

    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        std::string stripped = stripInlineComments(line, inBlockComment, inRawString, rawDelim);
        if (stripped.empty() && !inBlockComment && !inRawString) continue;
        if (isCommentLine(stripped)) continue;
        if (isPreprocessorLine(stripped)) continue;

        std::string masked = maskStringLiterals(stripped);

        // The maximum brace depth reached on this line. For a single-line
        // body such as `long f(long v){ g += v; return v; }` the depth rises
        // (entering the body) and falls (closing it) within one line; the
        // statements between the braces still belong to the function body and
        // must be scanned. We therefore (a) remember the peak depth so the
        // function-entry path below can place `functionDepth` correctly even
        // when the closing brace is on the same line, and (b) defer the
        // function-scope-exit caused by a same-line closing brace until AFTER
        // this line's write-scan, so a body that opens and closes on one line
        // is still scanned.
        int peakBraceDepth = braceDepth;
        bool deferredFunctionExit = false;

        // Track braces.
        for (size_t i = 0; i < masked.size(); ++i) {
            char c = masked[i];
            if (c == '{') {
                ++braceDepth;
                if (braceDepth > peakBraceDepth) peakBraceDepth = braceDepth;
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
                    // Defer the actual scope-exit: the body statements on this
                    // same line are scanned first, then the exit is applied at
                    // the end of the iteration.
                    deferredFunctionExit = true;
                }
            }
        }

        std::smatch nsMatch;
        if (std::regex_search(masked, nsMatch, nsRegex)) {
            nsStack.push(nsMatch[1].str());
            nsDepths.push(braceDepth - 1);
            continue;
        }

        std::smatch classMatch;
        if (!inFunction && std::regex_search(masked, classMatch, classRegex)) {
            classStack.push(classMatch[1].str());
            classDepths.push(braceDepth - 1);
            continue;
        }

        // Set true when the function body opens on THIS line. The body's
        // statements (and, for a compact single-line body, its closing brace)
        // are on this same line, so this line must be scanned regardless of
        // where the post-brace-loop `braceDepth` lands.
        bool enteredFunctionThisLine = false;

        if (pendingSignature && !inFunction && masked.find('{') != std::string::npos) {
            inFunction = true;
            // Use the peak depth seen on this line so the entry depth is
            // correct even when the matching `}` is on the same line and the
            // brace loop has already brought `braceDepth` back down.
            functionDepth = peakBraceDepth - 1;
            currentFunction = pendingFunctionName;
            pendingSignature = false;
            pendingFunctionName.clear();
            enteredFunctionThisLine = true;
        }

        std::smatch funcMatch;
        if (!inFunction && std::regex_search(masked, funcMatch, funcDefRegex)) {
            std::string fname = funcMatch[1].str();
            if (!isTypeKeyword(fname)) {
                bool hasBrace = masked.find('{') != std::string::npos;
                bool isForwardDecl = false;
                {
                    auto tail = masked.find_last_not_of(" \t");
                    if (tail != std::string::npos && masked[tail] == ';') {
                        isForwardDecl = true;
                    }
                }
                if (!isForwardDecl) {
                    if (hasBrace) {
                        inFunction = true;
                        // peak depth: correct even for a single-line body
                        // whose closing `}` already decremented braceDepth.
                        functionDepth = peakBraceDepth - 1;
                        currentFunction = fname;
                        enteredFunctionThisLine = true;
                    } else {
                        pendingSignature = true;
                        pendingFunctionName = fname;
                    }
                }
            }
        }

        // A complete single-line body opens AND closes on this line: we just
        // entered the function, yet the running depth is already back at or
        // below the opening depth (the closing `}` was on this same line).
        // Schedule the scope-exit so it is applied after this line is scanned
        // and `inFunction` does not leak into the next function.
        if (enteredFunctionThisLine && braceDepth <= functionDepth) {
            deferredFunctionExit = true;
        }

        // Only scan inside function bodies. Normally that means the running
        // `braceDepth` is strictly deeper than the function's opening depth.
        // The exception is a body that opens on this very line: its
        // statements are on this line even though `braceDepth` may have
        // already returned to (or below) `functionDepth` because the closing
        // `}` is on the same line. `enteredFunctionThisLine` keeps that
        // single-line / same-line case scannable without affecting the
        // multi-line path.
        if (!inFunction || (!enteredFunctionThisLine && braceDepth <= functionDepth)) {
            // Still apply any deferred same-line function-scope-exit so a
            // single-line body that we skip here does not leak `inFunction`.
            if (deferredFunctionExit) {
                inFunction = false;
                functionDepth = -1;
                currentFunction.clear();
            }
            continue;
        }

        std::string callerName = buildQualified(nsStack, classStack, currentFunction);

        // For each global, look for write candidates on the masked line.
        for (const auto& name : globals) {
            // Build regex patterns for this name.
            // Simple assignment: `\bname\s*=` (but not `==` / `!=` / `<=` / `>=`).
            // Compound assignment: `\bname\s*(\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=)`.
            // Pre/post increment: `\+\+name` or `name\+\+` / `--name` / `name--`.
            std::string escaped = name;

            // Word-boundary check manually because the name is captured once.
            size_t pos = 0;
            while (pos < masked.size()) {
                size_t found = masked.find(name, pos);
                if (found == std::string::npos) break;

                // Word boundary before
                bool leftOK = true;
                if (found > 0) {
                    char prev = masked[found - 1];
                    if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') leftOK = false;
                    // Member access: `obj.name` or `ptr->name`. Skip.
                    if (prev == '.') leftOK = false;
                    if (found >= 2 && prev == '>' && masked[found - 2] == '-') leftOK = false;
                    // Scope-qualified: `other::name` — we only track file-local globals
                    // so this is unlikely to match a genuine write, but let it through
                    // since the simple name matches.
                }
                // Word boundary after
                size_t end = found + name.size();
                bool rightOK = true;
                if (end < masked.size()) {
                    char nxt = masked[end];
                    if (std::isalnum(static_cast<unsigned char>(nxt)) || nxt == '_') rightOK = false;
                }

                if (!leftOK || !rightOK) {
                    pos = found + 1;
                    continue;
                }

                // Now check the right-hand context for a write operator.
                size_t after = end;
                // Skip whitespace
                while (after < masked.size() && (masked[after] == ' ' || masked[after] == '\t')) ++after;

                bool isWrite = false;
                if (after < masked.size()) {
                    char c = masked[after];
                    // `=` but not `==`
                    if (c == '=' && (after + 1 >= masked.size() || masked[after + 1] != '=')) {
                        isWrite = true;
                    }
                    // Compound: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`,
                    // `<<=`, `>>=`
                    if (!isWrite && after + 1 < masked.size() && masked[after + 1] == '=') {
                        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                            c == '&' || c == '|' || c == '^') {
                            isWrite = true;
                        }
                    }
                    if (!isWrite && after + 2 < masked.size() && masked[after + 2] == '=') {
                        if ((c == '<' && masked[after + 1] == '<') ||
                            (c == '>' && masked[after + 1] == '>')) {
                            isWrite = true;
                        }
                    }
                    // Postfix ++ / --
                    if (!isWrite && after + 1 < masked.size()) {
                        if ((c == '+' && masked[after + 1] == '+') ||
                            (c == '-' && masked[after + 1] == '-')) {
                            isWrite = true;
                        }
                    }
                }

                // Check the LEFT side for prefix ++/--: `++name` / `--name`.
                if (!isWrite && found >= 2) {
                    char p1 = masked[found - 1];
                    char p2 = masked[found - 2];
                    if ((p1 == '+' && p2 == '+') || (p1 == '-' && p2 == '-')) {
                        isWrite = true;
                    }
                }

                if (isWrite) {
                    SymbolAccess access;
                    access.function = callerName;
                    access.symbol = name;
                    access.isWrite = true;
                    access.file = filePath;
                    access.line = lineNum;
                    results.push_back(std::move(access));
                    // Only emit one write per global per line to avoid double-counting.
                    break;
                }
                pos = found + name.size();
            }
        }

        // Apply the deferred same-line function-scope-exit now that this
        // line's body statements have been scanned. This keeps a compact
        // single-line body (open + statements + close on one line) both
        // scanned AND correctly closed, so the next function is not
        // misattributed.
        if (deferredFunctionExit) {
            inFunction = false;
            functionDepth = -1;
            currentFunction.clear();
        }
    }

    return results;
}

} // namespace topo::check
