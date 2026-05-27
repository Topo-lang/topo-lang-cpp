#include "CppCallSiteExtractor.h"
#include "CppUnsafeCatalog.h"
#include "topo/Check/CapabilityCatalog.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <stack>
#include <string>

namespace topo::check {

namespace {

// Build qualified name from scope stacks
std::string buildQualified(const std::stack<std::string>& nsStack,
                           const std::stack<std::string>& classStack,
                           const std::string& funcName) {
    // Collect namespace parts
    std::vector<std::string> parts;
    {
        std::stack<std::string> tmp = nsStack;
        std::vector<std::string> ns;
        while (!tmp.empty()) { ns.push_back(tmp.top()); tmp.pop(); }
        for (auto it = ns.rbegin(); it != ns.rend(); ++it) parts.push_back(*it);
    }
    // Collect class parts
    {
        std::stack<std::string> tmp = classStack;
        std::vector<std::string> cls;
        while (!tmp.empty()) { cls.push_back(tmp.top()); tmp.pop(); }
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

// Build a caller name for global/namespace scope (not inside a function)
std::string buildGlobalCaller(const std::stack<std::string>& nsStack) {
    if (nsStack.empty()) return "<global>";
    std::stack<std::string> tmp = nsStack;
    std::vector<std::string> ns;
    while (!tmp.empty()) { ns.push_back(tmp.top()); tmp.pop(); }
    std::string result;
    for (auto it = ns.rbegin(); it != ns.rend(); ++it) {
        if (!result.empty()) result += "::";
        result += *it;
    }
    result += "::<global>";
    return result;
}

// Check if a line is a comment-only line
bool isCommentLine(const std::string& line) {
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos) return false;
    return (line.size() > pos + 1 && line[pos] == '/' && line[pos + 1] == '/');
}

// Check if a line is a preprocessor directive (other than #include/#define).
// These lines may contain braces that should not affect brace depth tracking.
// NOTE: #define is handled separately (F1.3) before this check is called.
bool isPreprocessorLine(const std::string& line) {
    size_t pos = line.find_first_not_of(" \t");
    if (pos == std::string::npos || line[pos] != '#') return false;
    std::string rest = line.substr(pos + 1);
    size_t kw = rest.find_first_not_of(" \t");
    if (kw == std::string::npos) return false;
    // Match preprocessor directives that can contain braces and corrupt counting
    return (rest.compare(kw, 2, "if") == 0 ||
            rest.compare(kw, 5, "ifdef") == 0 ||
            rest.compare(kw, 6, "ifndef") == 0 ||
            rest.compare(kw, 4, "else") == 0 ||
            rest.compare(kw, 4, "elif") == 0 ||
            rest.compare(kw, 5, "endif") == 0 ||
            rest.compare(kw, 6, "define") == 0);
}

} // anonymous namespace

std::vector<DetectedCallSite> CppCallSiteExtractor::extractCallSites(const std::string& filePath) {
    std::vector<DetectedCallSite> results;
    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    // Scope tracking (same pattern as CppSymbolExtractor)
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

    // Regex patterns
    std::regex nsRegex(R"(^\s*namespace\s+([\w:]+)\s*\{)");
    std::regex classRegex(R"(^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+(\w+)[^;]*\{)");
    // Simplified funcDefRegex: avoid overlapping quantifiers (ReDoS fix).
    // Match: optional qualifiers, then function name, then parameter list.
    std::regex funcDefRegex(R"(^\s*[\w:*&<>, ]+\s+(\w+)\s*\([^)]*\)\s*(?:const\s*)?(?:override\s*)?(?:final\s*)?\{?)");
    // Match known external API calls.
    // Only match bare calls or std::/:: prefixed calls.
    // User-namespace-qualified calls (e.g. app::read()) are filtered out in scanApiCalls.
    // F2.1: Expanded with ~45 additional system call names
    std::regex apiCallRegex(R"(\b(system|fork|execl|execv|execvp|execle|execve|execlp|popen|posix_spawn|socket|connect|bind|listen|accept|send|recv|sendto|recvfrom|sendmsg|recvmsg|getaddrinfo|gethostbyname|epoll_create|epoll_ctl|epoll_wait|poll|select|socketpair|pipe|mkfifo|fopen|fclose|fread|fwrite|freopen|tmpfile|open|close|read|write|lseek|mmap|munmap|pread|pwrite|mkstemp|sendfile|aio_read|aio_write|dlopen|dlsym|dlclose|LoadLibrary|GetProcAddress|setjmp|longjmp|_setjmp|_longjmp|waitpid|wait|kill|signal|ptrace|CreateProcess|_spawnl|pthread_create|thrd_create|dup|dup2|unlink|remove|rename|chmod|truncate|exit|_exit|_Exit|abort|raise|quick_exit|atexit|at_quick_exit|mprotect|VirtualAlloc|VirtualProtect|shmget|shmat|shmdt|shmctl|shm_open|shm_unlink|setuid|setgid|seteuid|setegid|chown|fchown|chroot|clone|vfork|daemon|setsid|prctl|sigaction|mkdir|rmdir|chdir|link|symlink|readlink|openat|stat|fstat|lstat|access|fchmod|ftruncate|creat|opendir|ioctl|fcntl|getenv|setenv|putenv|unsetenv|printf|fprintf|puts|fputs|sprintf|snprintf|malloc|calloc|realloc|reallocarray|free|aligned_alloc|posix_memalign|valloc|pvalloc|memalign|memcpy|memmove|memset|memchr|memcmp|bcopy|bzero|bcmp|strcpy|strncpy|strcat|strncat|strdup|strndup|construct_at|destroy_at|start_lifetime_as|start_lifetime_as_array)\s*\()");

    // F1.1: Constructor/destructor definition regex
    // Match: [qualifiers] [Scope::]Name(params) [: init-list] {
    // Also: [qualifiers] [Scope::]~Name(params) {
    static const std::regex ctorDtorRegex(R"(^\s*(?:[\w]+\s+)*(?:(\w+)::)?(~?\w+)\s*\([^)]*\)\s*(?::\s*[^{]*)?\{)");

    // F1.3: #define body extraction regex
    static const std::regex defineRegex(R"(define\s+(\w+)(?:\([^)]*\))?\s+(.*))");

    // --- Escape pattern regexes (compiled once, shared between scopes) ---

    // F1.5: Relaxed cast regex — keyword alone suffices (no < required)
    static const std::regex castRegex(R"(\b(reinterpret_cast|const_cast)\b)");
    // C-style cast: (type*)expr or (type)expr with pointer/numeric types
    static const std::regex cCastRegex(R"(\(\s*(void|char|int|long|short|unsigned|signed|float|double)\s*\*?\s*\))");
    // Inline assembly
    static const std::regex asmRegex(R"(\b(asm|__asm__|__asm)\b)");
    // volatile qualifier in expressions
    static const std::regex volatileRegex(R"(\bvolatile\b)");
    // goto statement
    static const std::regex gotoRegex(R"(\bgoto\s+\w+)");
    // std::thread construction
    static const std::regex threadRegex(R"(\bstd\s*::\s*thread\b)");

    // F1.4: __attribute__((constructor|destructor|cleanup|section|ifunc))
    static const std::regex attrRegex(R"(\b__attribute__\s*\(\s*\(\s*(constructor|destructor|cleanup|section|ifunc)\b)");
    // C++11 [[gnu::constructor]], [[gnu::destructor]]
    static const std::regex cxx11AttrRegex(R"(\[\[\s*gnu\s*::\s*(constructor|destructor)\s*\]\])");
    // __declspec(naked)
    static const std::regex declspecRegex(R"(\b__declspec\s*\(\s*naked\s*\))");

    // F2.3: New escape patterns
    // union in function body (type punning risk)
    static const std::regex unionRegex(R"(\bunion\b)");
    // placement new
    static const std::regex placementNewRegex(R"(\bnew\s*\()");
    // Dangerous __builtin_* functions
    static const std::regex builtinRegex(R"(\b__builtin_(return_address|frame_address)\b)");
    // std::async, std::jthread (implicit thread creation)
    static const std::regex asyncRegex(R"(\bstd\s*::\s*(async|jthread)\b)");
    // std::launder (pointer laundering)
    static const std::regex launderRegex(R"(\bstd\s*::\s*launder\b)");
    // std::bit_cast (type punning)
    static const std::regex bitcastRegex(R"(\bstd\s*::\s*bit_cast\b)");

    // F2.4: Address-of dangerous function detection
    static const std::regex addrOfRegex(R"(&\s*(system|fork|execv|execve|execvp|execl|execle|execlp|popen|dlopen|dlsym|socket|connect|bind|listen|accept|mprotect|setuid|setgid)\b)");

    // Dangerous function as template argument: call<system>("cmd")
    // Catches template<auto F> patterns where F is a known dangerous function
    static const std::regex templateArgDangerousRegex(R"(<\s*(system|fork|execv|execve|execvp|execl|execle|execlp|popen|dlopen|dlsym|socket|connect|bind|listen|accept|mprotect|setuid|setgid|kill|signal|ptrace|mmap|munmap)\s*>)");

    // Dangerous function name inside string literal — catches stringification
    // and dynamic dispatch: invoke_by_name("system"), dlsym(lib, "system")
    static const std::regex stringDangerousRegex(R"re("(system|execv|execve|execvp|execl|execle|execlp|fork|popen|dlopen|dlsym|socket|connect|bind|listen|accept|mprotect|setuid|setgid|kill|signal|ptrace|mmap|munmap)")re");

    // std::cout/cerr/clog usage (stream insertion)
    static const std::regex streamOutputRegex(R"(\bstd\s*::\s*(cout|cerr|clog)\b)");

    // F1.8: #pragma comment(lib, ...) link directive
    static const std::regex pragmaLinkRegex(R"(^\s*#\s*pragma\s+comment\s*\(\s*lib\b)");

    // --- Helper lambdas for shared scan logic ---

    auto emitSite = [&](const std::string& caller, const std::string& callee,
                        std::optional<CapabilityKind> cap, UnsafeLevel level, int ln) {
        DetectedCallSite site;
        site.callerQualifiedName = caller;
        site.calleePattern = callee;
        site.capability = cap;
        site.unsafeLevel = level;
        site.file = filePath;
        site.line = ln;
        results.push_back(std::move(site));
    };

    // Scan a line for API calls (apiCallRegex matches)
    // UnsafeCatalog is the primary gate.
    // Skip matches preceded by user namespace qualifiers (app::read()),
    // member access (obj.read()), or arrow access (ptr->read()).
    // Only bare calls and std::/:: prefixed calls are flagged.
    auto scanApiCalls = [&](const std::string& scanLine, const std::string& caller, int ln) {
        std::smatch apiMatch;
        std::string remaining = scanLine;
        size_t offset = 0; // track position in original scanLine
        while (std::regex_search(remaining, apiMatch, apiCallRegex)) {
            std::string callee = apiMatch[1].str();
            size_t matchPos = offset + static_cast<size_t>(apiMatch.position(1));

            // C3: Check what precedes the matched function name in the original line.
            // Skip if preceded by: user_ns:: (not std:: or bare ::), obj., ptr->
            bool skip = false;
            if (matchPos >= 2) {
                if (scanLine[matchPos - 1] == ':' && scanLine[matchPos - 2] == ':') {
                    if (matchPos >= 3) {
                        size_t nsEnd = matchPos - 2;
                        size_t nsStart = nsEnd;
                        while (nsStart > 0 && (std::isalnum(static_cast<unsigned char>(scanLine[nsStart - 1])) ||
                                                scanLine[nsStart - 1] == '_')) {
                            --nsStart;
                        }
                        std::string ns = scanLine.substr(nsStart, nsEnd - nsStart);
                        if (!ns.empty() && ns != "std") {
                            skip = true;
                        }
                    }
                }
            }
            if (!skip && matchPos >= 1) {
                if (scanLine[matchPos - 1] == '.') {
                    skip = true;
                }
            }
            if (!skip && matchPos >= 2) {
                if (scanLine[matchPos - 1] == '>' && scanLine[matchPos - 2] == '-') {
                    skip = true;
                }
            }

            if (!skip) {
                auto level = CppUnsafeCatalog::classifyCall(callee);
                if (level != UnsafeLevel::Safe) {
                    emitSite(caller, callee, classifyApiCall(callee), level, ln);
                }
            }
            offset += static_cast<size_t>(apiMatch.position()) + apiMatch.length();
            remaining = apiMatch.suffix().str();
        }
        // std::cout / std::cerr / std::clog stream insertion
        {
            std::smatch streamMatch;
            if (std::regex_search(scanLine, streamMatch, streamOutputRegex)) {
                std::string name = "std::" + streamMatch[1].str();
                emitSite(caller, name, CapabilityKind::File, UnsafeLevel::System, ln);
            }
        }
    };

    // Scan a line for escape patterns.
    // insideFunction: true when scanning inside a function body (enables function-only patterns).
    auto scanEscapes = [&](const std::string& scanLine, const std::string& caller, int ln,
                           bool insideFunction) {
        // F1.5: reinterpret_cast / const_cast
        {
            std::smatch castMatch;
            if (std::regex_search(scanLine, castMatch, castRegex)) {
                emitSite(caller, castMatch[1].str(), std::nullopt, UnsafeLevel::Escape, ln);
            }
        }
        // C-style cast to pointer type
        if (std::regex_search(scanLine, cCastRegex)) {
            emitSite(caller, "c-style-pointer-cast", std::nullopt, UnsafeLevel::Escape, ln);
        }
        // Inline assembly
        if (std::regex_search(scanLine, asmRegex)) {
            emitSite(caller, "inline-asm", std::nullopt, UnsafeLevel::Escape, ln);
        }
        // volatile
        if (insideFunction && std::regex_search(scanLine, volatileRegex)) {
            emitSite(caller, "volatile", std::nullopt, UnsafeLevel::Escape, ln);
        }
        // goto
        if (std::regex_search(scanLine, gotoRegex)) {
            emitSite(caller, "goto", std::nullopt, UnsafeLevel::Escape, ln);
        }
        // std::thread
        if (std::regex_search(scanLine, threadRegex)) {
            emitSite(caller, "std::thread", CapabilityKind::Process, UnsafeLevel::System, ln);
        }

        // F1.4: __attribute__ / [[gnu::*]] / __declspec
        if (std::regex_search(scanLine, attrRegex)) {
            emitSite(caller, "__attribute__", std::nullopt, UnsafeLevel::Escape, ln);
        }
        if (std::regex_search(scanLine, cxx11AttrRegex)) {
            emitSite(caller, "__attribute__", std::nullopt, UnsafeLevel::Escape, ln);
        }
        if (std::regex_search(scanLine, declspecRegex)) {
            emitSite(caller, "__declspec", std::nullopt, UnsafeLevel::Escape, ln);
        }

        // F2.3: union in function body only
        if (insideFunction && std::regex_search(scanLine, unionRegex)) {
            emitSite(caller, "union-in-function", std::nullopt, UnsafeLevel::Escape, ln);
        }
        // placement new
        if (std::regex_search(scanLine, placementNewRegex)) {
            emitSite(caller, "placement-new", std::nullopt, UnsafeLevel::Escape, ln);
        }
        // __builtin_return_address / __builtin_frame_address
        if (std::regex_search(scanLine, builtinRegex)) {
            emitSite(caller, "__builtin_dangerous", std::nullopt, UnsafeLevel::Escape, ln);
        }
        // std::async / std::jthread
        {
            std::smatch asyncMatch;
            if (std::regex_search(scanLine, asyncMatch, asyncRegex)) {
                std::string name = "std::" + asyncMatch[1].str();
                emitSite(caller, name, CapabilityKind::Process, UnsafeLevel::System, ln);
            }
        }
        // std::launder
        if (std::regex_search(scanLine, launderRegex)) {
            emitSite(caller, "std::launder", std::nullopt, UnsafeLevel::Escape, ln);
        }
        // std::bit_cast
        if (std::regex_search(scanLine, bitcastRegex)) {
            emitSite(caller, "std::bit_cast", std::nullopt, UnsafeLevel::Escape, ln);
        }

        // F2.4: Address-of dangerous function
        if (std::regex_search(scanLine, addrOfRegex)) {
            emitSite(caller, "addr-of-dangerous", std::nullopt, UnsafeLevel::Escape, ln);
        }

        // Template argument containing dangerous function: call<system>(...)
        {
            std::smatch tmplMatch;
            if (std::regex_search(scanLine, tmplMatch, templateArgDangerousRegex)) {
                emitSite(caller, "template-arg<" + tmplMatch[1].str() + ">",
                         std::nullopt, UnsafeLevel::Escape, ln);
            }
        }

        // Dangerous function name in string literal (stringification / dynamic dispatch)
        {
            std::smatch strMatch;
            std::string remaining = scanLine;
            while (std::regex_search(remaining, strMatch, stringDangerousRegex)) {
                emitSite(caller, "string-ref:\"" + strMatch[1].str() + "\"",
                         std::nullopt, UnsafeLevel::Escape, ln);
                remaining = strMatch.suffix().str();
            }
        }
    };

    // Combined scan: API calls + escape patterns
    auto scanForPatterns = [&](const std::string& scanLine, const std::string& caller, int ln,
                               bool insideFunction) {
        if (insideFunction) {
            scanApiCalls(scanLine, caller, ln);
        }
        scanEscapes(scanLine, caller, ln, insideFunction);
    };

    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        // --- State machine: raw string tracking (highest priority) ---
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
                // Content after */ on this line is processed below only if
                // it contains meaningful code. For simplicity and safety,
                // skip the entire line since mixed comment/code lines with
                // scope-affecting constructs on the tail are extremely rare.
            }
            continue;
        }

        // Skip comment lines
        if (isCommentLine(line)) continue;

        // F1.8: #pragma link directives (checked before preprocessor skip)
        if (std::regex_search(line, pragmaLinkRegex)) {
            std::string caller = inFunction
                ? buildQualified(nsStack, classStack, currentFunction)
                : buildGlobalCaller(nsStack);
            emitSite(caller, "pragma-link", std::nullopt, UnsafeLevel::Escape, lineNum);
        }

        // F1.3: #define body scanning (before general preprocessor skip)
        {
            size_t firstNonWs = line.find_first_not_of(" \t");
            if (firstNonWs != std::string::npos && line[firstNonWs] == '#') {
                std::string rest = line.substr(firstNonWs + 1);
                size_t kw = rest.find_first_not_of(" \t");
                if (kw != std::string::npos && rest.compare(kw, 6, "define") == 0) {
                    // Extract macro name and body
                    std::smatch defMatch;
                    if (std::regex_search(rest, defMatch, defineRegex)) {
                        std::string macroName = defMatch[1].str();
                        std::string macroBody = defMatch[2].str();

                        // Handle multi-line defines (backslash continuation)
                        while (!macroBody.empty() && macroBody.back() == '\\') {
                            macroBody.pop_back(); // remove backslash
                            std::string nextLine;
                            if (std::getline(file, nextLine)) {
                                ++lineNum;
                                // Trim trailing backslash from continuation
                                std::string trimmed = nextLine;
                                while (!trimmed.empty() &&
                                       (trimmed.back() == ' ' || trimmed.back() == '\t')) {
                                    trimmed.pop_back();
                                }
                                if (!trimmed.empty() && trimmed.back() == '\\') {
                                    macroBody += " " + trimmed.substr(0, trimmed.size() - 1);
                                    // Re-add backslash so the while loop continues
                                    macroBody += "\\";
                                } else {
                                    macroBody += " " + nextLine;
                                }
                            } else {
                                break;
                            }
                        }

                        std::string macroCaller = "<macro:" + macroName + ">";
                        scanForPatterns(macroBody, macroCaller, lineNum, /*insideFunction=*/true);
                    }
                    continue; // Skip brace tracking for #define lines
                }
            }
        }

        // Skip preprocessor directives that may contain braces
        if (isPreprocessorLine(line)) continue;

        // Scan for block comment / raw string openings that span the rest of
        // this line. If found, process the portion before the opening and then
        // enter the corresponding state.
        std::string effectiveLine = line;
        for (size_t i = 0; i < effectiveLine.size(); ++i) {
            char c = effectiveLine[i];
            // Line comment — truncate here
            if (c == '/' && i + 1 < effectiveLine.size() && effectiveLine[i + 1] == '/') {
                effectiveLine = effectiveLine.substr(0, i);
                break;
            }
            // Block comment start
            if (c == '/' && i + 1 < effectiveLine.size() && effectiveLine[i + 1] == '*') {
                auto closePos = effectiveLine.find("*/", i + 2);
                if (closePos != std::string::npos) {
                    // Same-line block comment: erase it and continue scanning
                    effectiveLine.erase(i, closePos + 2 - i);
                    --i;  // re-examine position after erase
                    continue;
                }
                // Multiline block comment — truncate line and enter state
                effectiveLine = effectiveLine.substr(0, i);
                inBlockComment = true;
                break;
            }
            // Raw string literal start: R"delimiter(
            if (c == 'R' && i + 1 < effectiveLine.size() && effectiveLine[i + 1] == '"') {
                auto parenPos = effectiveLine.find('(', i + 2);
                if (parenPos != std::string::npos) {
                    std::string delim = effectiveLine.substr(i + 2, parenPos - (i + 2));
                    std::string closer = ")" + delim + "\"";
                    auto closePos = effectiveLine.find(closer, parenPos + 1);
                    if (closePos != std::string::npos) {
                        // Same-line raw string: erase it
                        effectiveLine.erase(i, closePos + closer.size() - i);
                        --i;
                        continue;
                    }
                    // Multiline raw string — truncate and enter state
                    effectiveLine = effectiveLine.substr(0, i);
                    inRawString = true;
                    rawDelimiter = delim;
                    break;
                }
            }
        }

        // Track braces (on the effective line, with comments/raw strings removed)
        for (size_t i = 0; i < effectiveLine.size(); ++i) {
            char c = effectiveLine[i];
            // Skip characters inside string literals
            if (c == '"') {
                ++i;
                while (i < effectiveLine.size() && effectiveLine[i] != '"') {
                    if (effectiveLine[i] == '\\') ++i;  // skip escaped char
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
                // Guard against negative braceDepth (e.g., unbalanced preprocessor braces)
                if (braceDepth < 0) braceDepth = 0;
                // Pop namespace scope
                if (!nsDepths.empty() && braceDepth == nsDepths.top()) {
                    nsStack.pop();
                    nsDepths.pop();
                }
                // Pop class scope
                if (!classDepths.empty() && braceDepth == classDepths.top()) {
                    classStack.pop();
                    classDepths.pop();
                }
                // End function scope
                if (inFunction && braceDepth == functionDepth) {
                    inFunction = false;
                    functionDepth = -1;
                    currentFunction.clear();
                }
            }
        }

        // Detect namespace
        std::smatch nsMatch;
        if (std::regex_search(effectiveLine, nsMatch, nsRegex)) {
            nsStack.push(nsMatch[1].str());
            nsDepths.push(braceDepth - 1);
            continue;
        }

        // Detect class/struct
        std::smatch classMatch;
        if (!inFunction && std::regex_search(effectiveLine, classMatch, classRegex)) {
            classStack.push(classMatch[1].str());
            classDepths.push(braceDepth - 1);
            continue;
        }

        // Allman brace fix: if we have a pending signature and this line has {,
        // enter function scope now.
        if (pendingSignature && !inFunction && effectiveLine.find('{') != std::string::npos) {
            inFunction = true;
            functionDepth = braceDepth - 1;
            currentFunction = pendingFunctionName;
            pendingSignature = false;
            pendingFunctionName.clear();
        }

        // Detect function definition (only at class/namespace scope, not inside a function).
        // Skip #define lines to avoid matching macro function definitions.
        std::smatch funcMatch;
        if (!inFunction && std::regex_search(effectiveLine, funcMatch, funcDefRegex)) {
            // Skip if the original line is a #define
            size_t firstNonWs = line.find_first_not_of(" \t");
            bool isMacro = (firstNonWs != std::string::npos && line[firstNonWs] == '#');
            if (!isMacro) {
                std::string fname = funcMatch[1].str();
                // Filter out keywords that look like function names
                if (fname != "if" && fname != "for" && fname != "while" && fname != "switch" &&
                    fname != "return" && fname != "class" && fname != "struct" && fname != "namespace") {
                    if (effectiveLine.find('{') != std::string::npos) {
                        inFunction = true;
                        functionDepth = braceDepth - 1;
                        currentFunction = fname;
                    } else {
                        // Allman style: signature without { → wait for next line
                        pendingSignature = true;
                        pendingFunctionName = fname;
                    }
                }
            }
        }

        // F1.1: Constructor/destructor detection (if funcDefRegex didn't match)
        if (!inFunction && !pendingSignature) {
            std::smatch cdMatch;
            if (std::regex_search(effectiveLine, cdMatch, ctorDtorRegex)) {
                std::string className = cdMatch[1].str();
                std::string funcName = cdMatch[2].str();
                bool isCtor = (!className.empty() && funcName == className) ||
                              (!classStack.empty() && funcName == classStack.top());
                bool isDtor = !funcName.empty() && funcName[0] == '~';
                if (isCtor || isDtor) {
                    if (effectiveLine.find('{') != std::string::npos) {
                        inFunction = true;
                        functionDepth = braceDepth - 1;
                        currentFunction = funcName;
                    } else {
                        pendingSignature = true;
                        pendingFunctionName = funcName;
                    }
                }
            }
        }

        // Inside a function body: scan for API calls and escape patterns
        if (inFunction && braceDepth > functionDepth) {
            std::string callerName = buildQualified(nsStack, classStack, currentFunction);
            scanForPatterns(effectiveLine, callerName, lineNum, /*insideFunction=*/true);
        }

        // F1.2: Global scope scanning — detect dangerous patterns outside function bodies
        if (!inFunction) {
            std::string globalCaller = buildGlobalCaller(nsStack);
            scanForPatterns(effectiveLine, globalCaller, lineNum, /*insideFunction=*/false);
        }
    }

    return results;
}

} // namespace topo::check
