#include "ClangdBridge.h"

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"
#include "topo/Platform/ToolResolution.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>

namespace fs = std::filesystem;

namespace topo::lsp {

ClangdBridge::ClangdBridge() : LSPBridge("[topo-lsp]") {}

bool ClangdBridge::isClangdAvailable() {
    namespace plat = topo::platform;

    // Resolve clangd from the BYO LLVM toolchain (resolver derives the path
    // from the same root as the rest of the LLVM tools), falling back to a
    // bare name on PATH. Confirm via `clangd --version`, routed through
    // argv-style ``runProcessCapture`` — no shell, no quoting surface (sister
    // policy lives in ``scripts/audit/no-system-popen.sh``).
    std::string clangd = plat::llvmToolPath("clangd");
    if (clangd.empty()) clangd = "clangd" + std::string(plat::ExeSuffix);
    auto r = plat::runProcessCapture(clangd, {"--version"});
    return r.exitCode == 0;
}

bool ClangdBridge::start(const std::string& rootUri) {
    namespace fs = std::filesystem;
    std::string rootPath = uriToPath(rootUri);
    std::string buildDir = (fs::path(rootPath) / "build").string();
    return start(std::string{}, buildDir, rootUri);
}

bool ClangdBridge::start(const std::string& clangdPath, const std::string& buildDir, const std::string& rootUri) {
    namespace plat = topo::platform;

    // Determine clangd executable
    std::string exe = clangdPath;
    if (exe.empty()) {
        exe = plat::llvmToolPath("clangd");
        if (exe.empty()) exe = "clangd" + std::string(plat::ExeSuffix);
    }

    // Build arguments
    std::vector<std::string> args = {"--background-index", "--log=error"};
    if (!buildDir.empty()) {
        args.push_back("--compile-commands-dir=" + buildDir);
    }

    if (!startProcess(exe, args, rootUri))
        return false;

    parseSemanticTokenLegend();

    return true;
}

std::optional<SymbolResult> ClangdBridge::findDefinition(const std::string& qualifiedName,
                                                         const std::vector<std::string>& /*cppFiles*/) {
    if (!isAvailable()) return std::nullopt;
    return queryWorkspaceSymbol(qualifiedName);
}

std::vector<SymbolResult> ClangdBridge::findReferences(const std::string& qualifiedName,
                                                       const std::vector<std::string>& /*cppFiles*/) {
    if (!isAvailable()) return {};

    auto defn = queryWorkspaceSymbol(qualifiedName);
    if (!defn) return {};

    json params = {{"textDocument", {{"uri", pathToUri(defn->file)}}},
                   {"position", {{"line", defn->line}, {"character", defn->column}}},
                   {"context", {{"includeDeclaration", true}}}};

    auto response = sendRequest("textDocument/references", params);
    if (!response || !response->is_array()) return {};

    std::vector<SymbolResult> results;
    for (const auto& loc : *response) {
        SymbolResult r;
        r.file = uriToPath(loc["uri"].get<std::string>());
        r.line = loc["range"]["start"]["line"].get<int>();
        r.column = loc["range"]["start"]["character"].get<int>();
        results.push_back(std::move(r));
    }
    return results;
}

std::optional<std::string> ClangdBridge::getHoverInfo(const std::string& qualifiedName,
                                                      const std::vector<std::string>& /*cppFiles*/) {
    if (!isAvailable()) return std::nullopt;

    auto defn = queryWorkspaceSymbol(qualifiedName);
    if (!defn) return std::nullopt;

    json params = {{"textDocument", {{"uri", pathToUri(defn->file)}}},
                   {"position", {{"line", defn->line}, {"character", defn->column}}}};

    auto response = sendRequest("textDocument/hover", params);
    if (!response || response->is_null()) return std::nullopt;

    if (response->contains("contents")) {
        const auto& contents = (*response)["contents"];
        if (contents.is_string()) {
            return contents.get<std::string>();
        }
        if (contents.is_object() && contents.contains("value")) {
            return contents["value"].get<std::string>();
        }
    }
    return std::nullopt;
}

std::optional<SymbolResult> ClangdBridge::findTypeDefinition(const std::string& typeName,
                                                             const std::vector<std::string>& /*sourceFiles*/,
                                                             const std::vector<std::string>& includeDirs) {
    // Prefer the live index when clangd is running.
    if (isAvailable()) {
        auto result = queryWorkspaceSymbol(typeName);
        if (result) return result;
    }

    // Fallback: scan header files in includeDirs for a matching type
    // declaration.
    static const std::vector<std::string> kHeaderExts = {".h", ".hpp", ".hxx", ".hh"};
    // Matches: class Foo, struct Foo, enum Foo, union Foo,
    //          typedef ... Foo, using Foo =
    const std::regex pattern(R"((?:class|struct|enum(?:\s+class)?|union)\s+)" + typeName +
                             R"([\s:{;]|typedef\s+\S+\s+)" + typeName + R"([\s;]|using\s+)" + typeName + R"(\s*=)");

    for (const auto& dir : includeDirs) {
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
            if (ec || !entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            bool isHeader = false;
            for (const auto& hext : kHeaderExts) {
                if (ext == hext) {
                    isHeader = true;
                    break;
                }
            }
            if (!isHeader) continue;

            std::ifstream file(entry.path());
            if (!file.is_open()) continue;

            std::string line;
            int lineNo = 0;
            while (std::getline(file, line)) {
                ++lineNo;
                if (std::regex_search(line, pattern)) {
                    return SymbolResult{entry.path().string(), lineNo, 0};
                }
            }
        }
    }

    return std::nullopt;
}

} // namespace topo::lsp
