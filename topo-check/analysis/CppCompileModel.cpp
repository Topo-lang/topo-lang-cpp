#include "CppCompileModel.h"
#include "topo/Distribution/Sha256.h"
#include "topo/Platform/ToolResolution.h"
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <regex>

namespace topo::check {
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
constexpr size_t maxBytes = 8 * 1024 * 1024;
constexpr size_t maxArgumentBytes = 1024 * 1024;
constexpr size_t maxArguments = 16384;
std::string readBounded(const std::string& path, size_t limit) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + path);
    std::string result;
    char data[4096];
    while (stream.read(data, sizeof(data)) || stream.gcount()) {
        result.append(data, static_cast<size_t>(stream.gcount()));
        if (result.size() > limit) throw std::runtime_error("input exceeds size limit: " + path);
    }
    return result;
}
std::string absoluteInput(const std::string& path, const std::string& directory = "") {
    fs::path value(path);
    if (value.is_relative()) value = (directory.empty() ? fs::current_path() : fs::path(directory)) / value;
    // Resolve '..' through the filesystem: a preceding symlink can change
    // which parent it denotes. Preserve that spelling for reads and Clang.
    return value.string();
}
bool windowsCommand() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}
bool permittedInput(const std::string& path, const std::vector<std::string>& roots) {
    const auto physical = fs::weakly_canonical(path);
    for (const auto& root : roots) {
        const auto relative = physical.lexically_relative(fs::canonical(root));
        if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") return true;
    }
    return false;
}
void requireModelInput(const std::string& path, const CppCompileReadPolicy* policy) {
    if (policy && !permittedInput(path, policy->inputRoots))
        throw std::runtime_error("analysis input is outside the project and explicit input roots: " + path);
}
void expandResponses(const std::vector<std::string>& args, CppCompileUnit& unit,
                     std::vector<std::string>& expanded, std::set<std::string>& stack,
                     size_t& bytes, int depth, const CppCompileReadPolicy* policy) {
    if (depth > 8) throw std::runtime_error("response file nesting exceeds 8 levels");
    for (const auto& arg : args) {
        if (expanded.size() >= maxArguments) throw std::runtime_error("too many compiler arguments");
        if (arg.size() > 1 && arg.front() == '@') {
            auto path = absoluteInput(arg.substr(1), unit.directory);
            requireModelInput(path, policy);
            if (!stack.insert(path).second) throw std::runtime_error("response file cycle: " + path);
            auto text = readBounded(path, maxArgumentBytes);
            bytes += text.size();
            if (bytes > maxArgumentBytes) throw std::runtime_error("response input exceeds size limit");
            unit.inputs.push_back(path);
            unit.inputDigests[path] = topo::dist::sha256Hex(text);
            std::string error;
            auto inner = splitCppCommand(text, windowsCommand(), error);
            if (!error.empty()) throw std::runtime_error(error + " in " + path);
            expandResponses(inner, unit, expanded, stack, bytes, depth + 1, policy);
            stack.erase(path);
        } else {
            expanded.push_back(arg);
        }
    }
}
bool starts(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }
void sanitize(CppCompileUnit& u, std::map<std::string, std::string>& digestCache, const CppCompileReadPolicy* policy) {
    if (u.originalArguments.empty()) throw std::runtime_error("empty compiler argv");
    auto compiler = fs::path(u.originalArguments[0]).filename().string();
    if (compiler != "clang" && compiler != "clang++" && compiler != "clang.exe" && compiler != "clang++.exe" &&
        !starts(compiler, "clang-") && !starts(compiler, "clang++-"))
        throw std::runtime_error("unsupported compiler driver (Clang-compatible model required): " + compiler);
    u.compiler = u.originalArguments[0];
    if (fs::path(u.compiler).is_relative() && fs::path(u.compiler).has_parent_path())
        u.compiler = absoluteInput(u.compiler, u.directory);
    else if (!fs::path(u.compiler).has_parent_path()) {
        auto resolved = topo::platform::findOnPath(u.compiler);
        if (!resolved.empty()) u.compiler = resolved;
    }
    if (policy) {
        const auto canonical = cppCanonicalPath(u.compiler);
        if (std::find(policy->compilerFiles.begin(), policy->compilerFiles.end(), canonical) == policy->compilerFiles.end())
            throw std::runtime_error("compile model compiler is not an independently discovered installed native Clang: " + u.compiler);
    }
    if (!fs::is_regular_file(u.compiler))
        throw std::runtime_error("compiler input unavailable: " + u.compiler);
    std::vector<std::string> defaults;
    const bool explicitNoDefault = std::find(u.originalArguments.begin(), u.originalArguments.end(), "--no-default-config") != u.originalArguments.end();
    if (fs::is_regular_file(u.compiler)) {
        u.compiler = fs::absolute(u.compiler).string();
        u.inputs.push_back(u.compiler);
        auto digest = digestCache.find(u.compiler);
        if (digest == digestCache.end()) digest = digestCache.emplace(u.compiler, topo::dist::sha256File(u.compiler)).first;
        u.inputDigests[u.compiler] = digest->second;
        const auto configDirectory = fs::path(cppCanonicalPath(u.compiler)).parent_path().parent_path() / "etc/clang";
        std::vector<fs::path> configs;
        std::error_code ec;
        if (!explicitNoDefault && fs::is_directory(configDirectory, ec))
            for (const auto& file : fs::directory_iterator(configDirectory))
                if (file.is_regular_file() && file.path().extension() == ".cfg") configs.push_back(file.path());
        if (!configs.empty())
            throw std::runtime_error("implicit Clang configuration requires explicitly expanded arguments and --no-default-config");
    }
    if (!policy || !policy->fixedEnvironment) if (const auto* home = std::getenv("HOME")) {
        std::error_code ec;
        const auto userConfig = fs::path(home) / ".config/clang";
        if (!explicitNoDefault && fs::is_directory(userConfig, ec))
            for (const auto& file : fs::directory_iterator(userConfig))
                if (file.is_regular_file() && file.path().extension() == ".cfg")
                    throw std::runtime_error("user Clang configuration must be expanded into the explicit compile model");
    }
    std::vector<std::string> expanded;
    std::set<std::string> stack;
    size_t bytes = 0;
    defaults.insert(defaults.end(), u.originalArguments.begin() + 1, u.originalArguments.end());
    expandResponses(defaults, u, expanded, stack, bytes, 0, policy);
    const std::set<std::string> ignored = {"-c", "-S", "-E", "-MD", "-MMD", "-MP", "-M", "-MM", "-MG", "-emit-llvm"};
    const std::set<std::string> ignoredValue = {"-o", "-MF", "-MT", "-MQ", "-MJ"};
    const std::set<std::string> pathOptions = {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros", "-isysroot", "--sysroot", "-resource-dir", "-F", "-iframework"};
    const std::set<std::string> valueOptions = {"-D", "-U", "-x", "-target", "--target", "-arch", "-std", "-stdlib"};
    const std::set<std::string> flags = {"--no-default-config", "-pthread", "-fPIC", "-fpic", "-fPIE", "-fpie", "-fno-exceptions", "-fexceptions", "-frtti", "-fno-rtti", "-fshort-wchar", "-funsigned-char", "-fsigned-char", "-nostdinc", "-nostdinc++", "-m32", "-m64", "-pedantic", "-pedantic-errors", "-fsyntax-only", "-fvisibility-inlines-hidden", "-fno-omit-frame-pointer"};
    for (size_t i = 0; i < expanded.size(); ++i) {
        const auto& arg = expanded[i];
        if (arg.empty()) continue;
        if (arg.front() != '-' && cppCanonicalPath(arg, u.directory) == cppCanonicalPath(u.file)) continue;
        auto value = [&]() -> const std::string& {
            if (++i >= expanded.size()) throw std::runtime_error("missing value for " + arg);
            return expanded[i];
        };
        if (ignored.count(arg)) continue;
        if (arg=="-Xclang" && i+1<expanded.size() && expanded[i+1]=="-disable-llvm-passes") { ++i; continue; }
        if (arg=="-fno-autolink") { u.arguments.push_back(arg); continue; }
        if (ignoredValue.count(arg)) { value(); continue; }
        if (pathOptions.count(arg)) {
            auto path = absoluteInput(value(), u.directory);
            requireModelInput(path, policy);
            u.arguments.push_back(arg); u.arguments.push_back(path);
            if (arg == "-include" || arg == "-imacros") u.inputs.push_back(path);
            continue;
        }
        if (valueOptions.count(arg)) { u.arguments.push_back(arg); u.arguments.push_back(value()); continue; }
        if (starts(arg, "-I") || starts(arg, "-F")) {
            auto path = absoluteInput(arg.substr(2), u.directory);
            requireModelInput(path, policy);
            u.arguments.push_back(arg.substr(0, 2) + path); continue;
        }
        if (starts(arg, "--sysroot=") || starts(arg, "-resource-dir=")) {
            auto eq = arg.find('=');
            auto path = absoluteInput(arg.substr(eq + 1), u.directory);
            requireModelInput(path, policy);
            u.arguments.push_back(arg.substr(0, eq + 1) + path); continue;
        }
        if (starts(arg, "-D") || starts(arg, "-U") || starts(arg, "-std=") || starts(arg, "--target=") ||
            starts(arg, "-stdlib=") || starts(arg, "-mmacosx-version-min=") || starts(arg, "-fvisibility=") ||
            starts(arg, "-march=") || starts(arg, "-mcpu=") || starts(arg, "-mtune=") || flags.count(arg)) {
            u.arguments.push_back(arg); continue;
        }
        if (starts(arg, "-W") || arg == "-w" || starts(arg, "-O") || arg == "-g" || arg == "-g0" || arg == "-g1" || arg == "-g2" || arg == "-g3" || starts(arg, "-gdwarf-")) {
            // Driver forwarding flags can load code or change semantics.
            if (starts(arg, "-Wl,") || starts(arg, "-Wa,") || starts(arg, "-Wp,"))
                throw std::runtime_error("unsupported forwarded option: " + arg);
            u.arguments.push_back(arg); continue;
        }
        throw std::runtime_error("unsupported compilation option: " + arg);
    }
    u.arguments.push_back("--no-default-config");
    u.arguments.push_back("-working-directory"); u.arguments.push_back(u.directory);
}
} // namespace

std::string cppCanonicalPath(const std::string& path, const std::string& directory) {
    fs::path p(path);
    if (p.is_relative()) p = (directory.empty() ? fs::current_path() : fs::path(directory)) / p;
    std::error_code ec;
    auto real = fs::weakly_canonical(p, ec);
    return (ec ? p.lexically_normal() : real).string();
}
std::vector<std::string> splitCppCommand(const std::string& text, bool windows, std::string& error) {
    std::vector<std::string> result;
    std::string token;
    char quote = 0;
    bool active = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\0') { error = "NUL in command"; return {}; }
        if (windows && c == '\\') {
            size_t start = i;
            while (i < text.size() && text[i] == '\\') ++i;
            size_t count = i - start;
            if (i < text.size() && text[i] == '"') {
                token.append(count / 2, '\\');
                if (count % 2) token += '"'; else quote = quote ? 0 : '"';
            } else { token.append(count, '\\'); --i; }
            active = true; continue;
        }
        if (!windows && c == '\\' && quote != '\'') {
            if (++i >= text.size()) { error = "dangling escape in command"; return {}; }
            token += text[i]; active = true; continue;
        }
        if (c == '"' || (!windows && c == '\'')) {
            if (!quote) quote = c;
            else if (quote == c) quote = 0;
            else token += c;
            active = true; continue;
        }
        if (!quote && std::isspace(static_cast<unsigned char>(c))) {
            if (active) { result.push_back(token); token.clear(); active = false; }
            continue;
        }
        if (!windows && ((quote != '\'' && (c == '$' || c == '`')) ||
                         (!quote && std::string(";&|<>()").find(c) != std::string::npos))) {
            error = "shell evaluation is unsupported in compile command"; return {};
        }
        token += c; active = true;
    }
    if (quote) { error = "unterminated quote in command"; return {}; }
    if (active) result.push_back(token);
    return result;
}
CppCompileModel loadCppCompileModel(const std::string& path, const std::string& project, const CppCompileReadPolicy* policy) {
    CppCompileModel model;
    model.path = absoluteInput(path);
    model.project = project.empty() ? fs::path(model.path).parent_path().string() : cppCanonicalPath(project);
    model.inputs.push_back(model.path);
    try {
        requireModelInput(model.path, policy);
        const auto source = readBounded(model.path, maxBytes);
        model.inputDigests[model.path] = topo::dist::sha256Hex(source);
        auto root = json::parse(source);
        if (!root.is_array()) throw std::runtime_error("compile database must be a JSON array");
        if (root.size() > 100000) throw std::runtime_error("compile database exceeds TU limit");
        std::map<std::string, std::string> digestCache;
        for (size_t index = 0; index < root.size(); ++index) {
            CppCompileUnit unit;
            unit.variant = std::to_string(index);
            try {
                const auto& entry = root[index];
                unit.directory = absoluteInput(entry.at("directory").get<std::string>(), fs::path(model.path).parent_path().string());
                unit.file = absoluteInput(entry.at("file").get<std::string>(), unit.directory);
                requireModelInput(unit.directory, policy);
                requireModelInput(unit.file, policy);
                if (!fs::is_regular_file(unit.file)) throw std::runtime_error("source file missing: " + unit.file);
                if (entry.contains("arguments")) unit.originalArguments = entry.at("arguments").get<std::vector<std::string>>();
                else {
                    std::string error;
                    unit.originalArguments = splitCppCommand(entry.at("command").get<std::string>(), windowsCommand(), error);
                    if (!error.empty()) throw std::runtime_error(error);
                }
                sanitize(unit, digestCache, policy);
            } catch (const std::exception& e) { unit.errors.push_back(e.what()); }
            model.units.push_back(std::move(unit));
        }
        if (model.units.empty()) model.errors.push_back("compile database contains no translation units");
    } catch (const std::exception& e) { model.errors.push_back(e.what()); }
    return model;
}
bool CppCompileModel::complete() const {
    return errors.empty() && !units.empty() && std::all_of(units.begin(), units.end(), [](const auto& unit) { return unit.errors.empty(); });
}

namespace {
// This scanner over-approximates literal and finite object-macro lookups,
// including inactive branches and lower-priority candidates. It never infers dependency closure
// merely from the headers Clang opened on a previous run.
std::string cachePreprocessingText(std::string text) {
    // Translation phase one normalizes physical line endings before phase
    // two removes escaped newlines. Input digests still bind the original bytes.
    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            normalized += '\n';
        } else normalized += text[i];
    }
    std::string spliced;
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i] == '\\' && i + 1 < normalized.size() && normalized[i + 1] == '\n') { ++i; continue; }
        spliced += normalized[i];
    }
    if (spliced.find_first_of("%?\\") != std::string::npos || spliced.find('\0') != std::string::npos ||
        spliced.find("import") != std::string::npos || spliced.find("R\"") != std::string::npos || spliced.find("_Pragma") != std::string::npos ||
        spliced.find("__pragma") != std::string::npos || spliced.find("__has_embed") != std::string::npos || spliced.find("__DATE__") != std::string::npos ||
        spliced.find("__TIME__") != std::string::npos || spliced.find("__TIMESTAMP__") != std::string::npos)
        throw std::runtime_error("dynamic or unsupported preprocessor syntax requires uncached analysis");
    // In a header-name token these bytes are literal, whereas in ordinary C++
    // they start comments. Refuse the ambiguous spelling before stripping.
    if (std::regex_search(spliced, std::regex(R"(<[^\r\n>]*(/\*|//)[^\r\n>]*>)")))
        throw std::runtime_error("comment delimiter in angle header requires uncached analysis");
    // Preserve strings/header names and line boundaries while removing comments.
    std::string clean;
    char quote = 0;
    for (size_t i = 0; i < spliced.size(); ++i) {
        const char c = spliced[i];
        if (quote) { clean += c; if (c == quote || c == '\n') quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; clean += c; continue; }
        if (c == '/' && i + 1 < spliced.size() && spliced[i + 1] == '/') {
            clean += ' '; i += 2;
            while (i < spliced.size() && spliced[i] != '\n') ++i;
            if (i < spliced.size()) clean += '\n';
        } else if (c == '/' && i + 1 < spliced.size() && spliced[i + 1] == '*') {
            clean += ' '; i += 2;
            while (i + 1 < spliced.size() && !(spliced[i] == '*' && spliced[i + 1] == '/')) {
                if (spliced[i] == '\n') clean += '\n';
                ++i;
            }
            if (i + 1 >= spliced.size()) throw std::runtime_error("unterminated comment in cache input");
            ++i;
        } else clean += c;
    }
    if (std::regex_search(clean, std::regex(R"(#\s*#)")))
        throw std::runtime_error("preprocessor token paste requires uncached analysis");
    return clean;
}
struct LiteralLookup { std::string name; bool quoted; };
using HeaderMacros = std::map<std::string, std::set<std::string>>;
constexpr size_t maxHeaderMacros = 4096;
constexpr size_t maxHeaderReplacements = 64;
constexpr size_t maxHeaderAliasDepth = 64;
constexpr size_t maxHeaderExpansions = 262144;
constexpr size_t maxHeaderExpansionBytes = 64 * 1024 * 1024;
constexpr size_t maxHeaderClosureRounds = 128;
std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
bool identifier(const std::string& text) {
    return std::regex_match(text, std::regex(R"([A-Za-z_][A-Za-z_0-9]*)"));
}
void addHeaderMacro(HeaderMacros& macros, const std::string& definition, bool replace = false) {
    std::smatch match;
    if (!std::regex_match(definition, match, std::regex(R"(^\s*([A-Za-z_][A-Za-z_0-9]*)(.*)$)")))
        throw std::runtime_error("unclassified macro definition requires uncached analysis");
    auto& replacements = macros[match[1]];
    if (replace) replacements.clear();
    const auto tail = match[2].str();
    replacements.insert(!tail.empty() && tail.front() == '(' ? "@function-like" : trimmed(tail));
    if (macros.size() > maxHeaderMacros || replacements.size() > maxHeaderReplacements)
        throw std::runtime_error("header macro definition limit exceeded");
}
LiteralLookup literalLookup(const std::string& operand) {
    const auto text = trimmed(operand);
    if (text.empty() || (text.front() != '<' && text.front() != '"'))
        throw std::runtime_error("unsupported include operand requires uncached analysis");
    const char end = text.front() == '<' ? '>' : '"';
    const auto close = text.find(end, 1);
    if (close == std::string::npos || close + 1 != text.size())
        throw std::runtime_error("unsupported include operand requires uncached analysis");
    auto name = text.substr(1, close - 1);
    const fs::path path(name);
    if (name.empty() || path.is_absolute()) throw std::runtime_error("absolute or empty include requires uncached analysis");
    for (const auto& component : path)
        if (component == ".." || component == ".") throw std::runtime_error("include path traversal requires uncached analysis");
    return {name, text.front() == '"'};
}
std::vector<std::string> headerLookups(const std::string& source, HeaderMacros& macros) {
    const auto clean = cachePreprocessingText(source);
    std::vector<std::string> result;
    std::istringstream lines(clean);
    std::string line;
    const std::regex directive(R"(^\s*#\s*([A-Za-z_][A-Za-z_0-9]*)(.*)$)");
    while (std::getline(lines, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, directive)) {
            if (line.find('#') != std::string::npos)
                throw std::runtime_error("unclassified directive marker requires uncached analysis");
            continue;
        }
        const std::string kind = match[1];
        if (kind == "include" || kind == "include_next") result.push_back(trimmed(match[2]));
        else if (kind == "define") {
            if (match[2].str().find("__has_include") != std::string::npos)
                throw std::runtime_error("macro-generated include probe requires uncached analysis");
            addHeaderMacro(macros, match[2]);
        } else if (kind == "undef") {
            const auto name = trimmed(match[2]);
            if (name == "__has_include" || name == "__has_include_next")
                throw std::runtime_error("overridden include probe requires uncached analysis");
            if (!identifier(name)) throw std::runtime_error("unsupported macro undefinition requires uncached analysis");
            // Without evaluating branch/order state, a source undefinition of
            // a participating header macro cannot establish a closed expansion.
            macros[name].insert("@undefined");
            if (macros.size() > maxHeaderMacros || macros[name].size() > maxHeaderReplacements)
                throw std::runtime_error("header macro definition limit exceeded");
        } else if (kind == "pragma" && !std::regex_match(match[2].str(), std::regex(R"(\s*once\s*)")))
            throw std::runtime_error("pragma other than once requires uncached analysis");
        else if (kind == "embed") throw std::runtime_error("embedded file requires uncached analysis");
        else if (kind != "pragma" && kind != "if" && kind != "ifdef" && kind != "ifndef" &&
                 kind != "elif" && kind != "else" && kind != "endif" && kind != "line" &&
                 kind != "error" && kind != "warning")
            throw std::runtime_error("unsupported preprocessor directive requires uncached analysis: " + kind);
    }
    const std::regex hasInclude(R"(\b__has_include(?:_next)?\b)");
    for (auto it = std::sregex_iterator(clean.begin(), clean.end(), hasInclude); it != std::sregex_iterator(); ++it) {
        auto start = clean.find_first_not_of(" \t\r\n", static_cast<size_t>(it->position() + it->length()));
        if (start == std::string::npos || clean[start] != '(')
            throw std::runtime_error("aliased include probe requires uncached analysis");
        auto end = clean.find(')', start + 1);
        if (end == std::string::npos) throw std::runtime_error("unterminated include probe");
        result.push_back(trimmed(clean.substr(start + 1, end - start - 1)));
    }
    return result;
}
bool expandHeaderLookup(const std::string& operand, const HeaderMacros& macros,
                        std::vector<LiteralLookup>& result, std::set<std::string>& active,
                        size_t& expansions, size_t& expansionBytes, bool replacement = false) {
    if (operand.size() > maxHeaderExpansionBytes - expansionBytes)
        throw std::runtime_error("header macro expansion text limit exceeded");
    expansionBytes += operand.size();
    if (++expansions > maxHeaderExpansions || active.size() > maxHeaderAliasDepth)
        throw std::runtime_error("header macro expansion limit exceeded");
    if (!operand.empty() && (operand.front() == '"' || operand.front() == '<')) {
        // A quoted string is one preprocessing token. Angle replacement lists
        // undergo further token expansion, unlike a direct #include <...>.
        if (replacement && operand.front() == '<')
            throw std::runtime_error("angle-token header macro requires uncached analysis");
        result.push_back(literalLookup(operand));
        return true;
    }
    if (!identifier(operand)) throw std::runtime_error("complex header macro requires uncached analysis");
    const auto found = macros.find(operand);
    if (found == macros.end()) return false; // Another candidate header may supply it.
    if (!active.insert(operand).second) throw std::runtime_error("cyclic header macro requires uncached analysis");
    bool complete = true;
    for (const auto& value : found->second)
        if (!expandHeaderLookup(value, macros, result, active, expansions, expansionBytes, true)) complete = false;
    active.erase(operand);
    return complete;
}
}

json cppAuditInputIdentity(const CppCompileModel& model, const CppCompileReadPolicy* policy) {
    try {
        if (!model.complete()) throw std::runtime_error("compilation model is incomplete");
        if (!policy || !policy->fixedEnvironment) for (const auto* name : {"CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "OBJCPLUS_INCLUDE_PATH",
                                 "SDKROOT", "CCC_OVERRIDE_OPTIONS", "CCC_ADD_ARGS"})
            if (const auto* value = std::getenv(name); value && *value)
                throw std::runtime_error(std::string("implicit include environment requires uncached analysis: ") + name);
        json inputs = json::object(), candidates = json::object(), units = json::array();
        size_t bytes = 0, scannedFiles = 0;
        auto describe = [&](const std::string& path) {
            if (policy && std::find(policy->compilerFiles.begin(), policy->compilerFiles.end(), cppCanonicalPath(path)) == policy->compilerFiles.end())
                requireModelInput(path, policy);
            if (candidates.size() >= 32768 && !candidates.contains(path)) throw std::runtime_error("header candidate limit exceeded");
            const auto status = fs::status(path);
            json state = {{"resolvedPath", fs::weakly_canonical(path).generic_string()}, {"kind", "missing"}};
            if (fs::is_regular_file(status)) state["kind"] = "file";
            else if (fs::is_directory(status)) state["kind"] = "directory";
            else if (fs::exists(status)) throw std::runtime_error("special file in include search: " + path);
            // weakly_canonical observes the longest existing parent even when
            // the candidate is absent, including intermediate symlink changes.
            state["parentResolvedPath"] = fs::weakly_canonical(fs::path(path).parent_path()).generic_string();
            // Also bind dangling symlink spellings: weakly_canonical cannot
            // resolve their absent targets, but retargeting is still an input
            // change and must be visible before a target later appears.
            state["symlinks"] = json::array();
            fs::path prefix = fs::path(path).root_path();
            for (const auto& component : fs::path(path).relative_path()) {
                prefix /= component;
                std::error_code error;
                const auto linkStatus = fs::symlink_status(prefix, error);
                if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
                    throw std::runtime_error("cannot inspect include path: " + prefix.string());
                if (fs::is_symlink(linkStatus)) state["symlinks"].push_back({{"path", prefix.generic_string()}, {"target", fs::read_symlink(prefix).generic_string()}});
                if (!fs::exists(linkStatus)) break;
            }
            candidates[path] = state;
            return fs::is_regular_file(status);
        };
        auto capture = [&](const std::string& file, bool scan) {
            if (!describe(file)) throw std::runtime_error("cache input unavailable: " + file);
            if (fs::file_size(file) > (scan ? 8ULL * 1024 * 1024 : 1024ULL * 1024 * 1024))
                throw std::runtime_error("cache input exceeds size limit: " + file);
            std::string contents;
            if (scan) {
                // pragma once observes physical file equivalence. Content and
                // canonical paths alone cannot distinguish hardlink changes.
                if (fs::hard_link_count(file) > 1) throw std::runtime_error("hardlinked preprocessing input requires uncached analysis: " + file);
                contents = readBounded(file, 8 * 1024 * 1024);
                bytes += contents.size();
                if (++scannedFiles > 4096 || bytes > 64 * 1024 * 1024) throw std::runtime_error("header closure limit exceeded");
            }
            const auto digest = scan ? topo::dist::sha256Hex(contents) : topo::dist::sha256File(file);
            if (digest.empty()) throw std::runtime_error("cannot hash cache input: " + file);
            inputs[file] = {{"resolvedPath", fs::canonical(file).generic_string()}, {"sha256", digest}};
            return contents;
        };
        capture(model.path, false);
        for (const auto& [path, digest] : model.inputDigests)
            if (inputs.at(path).at("sha256") != digest) throw std::runtime_error("compile model changed during cache capture");
        for (const auto& unit : model.units) {
            const auto& args = unit.arguments;
            for (const auto* required : {"-std=c++17", "-nostdinc", "-nostdinc++"})
                if (std::find(args.begin(), args.end(), required) == args.end())
                    throw std::runtime_error("cache requires explicit C++17 and disabled implicit standard include search");
            std::vector<std::string> roots, quotedRoots, forced;
            HeaderMacros macros;
            for (size_t i = 0; i < args.size(); ++i) {
                const auto& arg = args[i];
                if (arg == "-I" || arg == "-isystem" || arg == "-idirafter" || arg == "-iquote") {
                    const auto& root = args.at(++i);
                    (arg == "-iquote" ? quotedRoots : roots).push_back(root);
                    describe(root);
                } else if (starts(arg, "-I")) { roots.push_back(arg.substr(2)); describe(roots.back()); }
                else if (arg == "-include" || arg == "-imacros") forced.push_back(args.at(++i));
                else if (arg == "-working-directory") { ++i; }
                else if (arg == "-D" || arg == "-U" || starts(arg, "-D") || starts(arg, "-U")) {
                    const bool define = starts(arg, "-D");
                    const auto value = arg.size() == 2 ? args.at(++i) : arg.substr(2);
                    const auto clean = cachePreprocessingText(value);
                    if (clean.find("__has_include") != std::string::npos || clean.find('\n') != std::string::npos)
                        throw std::runtime_error("compiler macro contains an include probe or directive");
                    if (define) {
                        const auto equals = clean.find('=');
                        addHeaderMacro(macros, equals == std::string::npos ? clean + " 1" :
                            clean.substr(0, equals) + " " + clean.substr(equals + 1), true);
                    } else {
                        if (!identifier(clean)) throw std::runtime_error("unsupported compiler macro undefinition");
                        macros.erase(clean);
                    }
                }
                else if (arg != "--no-default-config" && arg != "-std=c++17" && arg != "-nostdinc" && arg != "-nostdinc++")
                    throw std::runtime_error("compilation option requires uncached dependency discovery: " + arg);
            }
            units.push_back({{"file", unit.file}, {"directory", unit.directory}, {"arguments", args}});
            for (const auto& input : unit.inputs) capture(input, false);
            for (const auto& [path, digest] : unit.inputDigests)
                if (inputs.at(path).at("sha256") != digest) throw std::runtime_error("compile input changed during cache capture: " + path);
            std::set<std::string> visited, expandedCandidates;
            std::vector<std::string> pending{unit.file};
            pending.insert(pending.end(), forced.begin(), forced.end());
            std::vector<std::pair<std::string, std::string>> lookups;
            size_t expansions = 0, expansionBytes = 0;
            // All branches and all definitions form a conservative superset.
            // New candidate headers can add aliases or alternatives to earlier
            // lookups, so close the graph before accepting any cache identity.
            size_t rounds = 0;
            while (true) {
                if (++rounds > maxHeaderClosureRounds) throw std::runtime_error("header closure round limit exceeded");
                while (!pending.empty()) {
                    const auto file = pending.back(); pending.pop_back();
                    if (!visited.insert(file).second) continue;
                    for (const auto& operand : headerLookups(capture(file, true), macros)) {
                        if (lookups.size() >= 32768) throw std::runtime_error("header lookup limit exceeded");
                        lookups.emplace_back(file, operand);
                    }
                }
                bool complete = true;
                for (const auto& [file, operand] : lookups) {
                    std::vector<LiteralLookup> resolved;
                    std::set<std::string> active;
                    if (!expandHeaderLookup(operand, macros, resolved, active, expansions, expansionBytes)) complete = false;
                    for (const auto& lookup : resolved) {
                        std::vector<std::string> search = roots;
                        if (lookup.quoted) {
                            search.insert(search.end(), quotedRoots.begin(), quotedRoots.end());
                            search.push_back(fs::path(file).parent_path().string());
                            search.push_back(fs::canonical(file).parent_path().string());
                        }
                        for (const auto& root : search) {
                            const auto candidate = absoluteInput(lookup.name, root);
                            if (expandedCandidates.insert(candidate).second && describe(candidate) && !visited.count(candidate))
                                pending.push_back(candidate);
                        }
                    }
                }
                if (pending.empty()) {
                    if (!complete) throw std::runtime_error("undefined header macro requires uncached analysis");
                    break;
                }
            }
        }
        return {{"supported", true}, {"inputs", inputs}, {"searchCandidates", candidates}, {"units", units}};
    } catch (const std::exception& error) { return {{"supported", false}, {"reason", error.what()}}; }
}
} // namespace topo::check
