#include "CppCMakeModel.h"
#include "CppCompileModel.h"
#include "topo/Distribution/Sha256.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include <algorithm>
namespace topo::check {
namespace fs = std::filesystem;
using json = nlohmann::json;
CppCMakeModel readCppCMakeModel(const std::string& buildDirectory, const std::string& project) {
    CppCMakeModel result;
    const auto reply = fs::path(buildDirectory) / ".cmake/api/v1/reply";
    std::error_code ec;
    if (!fs::is_directory(reply, ec)) {
        if (fs::is_regular_file(fs::path(project) / "CMakeLists.txt", ec))
            result.unresolved.push_back("CMake File API reply unavailable; request codemodel-v2 and cmakeFiles-v1 in the configured build, then pass its compile_commands.json (configuration scripts are not run by audit)");
        return result;
    }
    result.present = true;
    auto read = [&](const fs::path& path) {
        if (!fs::is_regular_file(path) || fs::file_size(path) > 8 * 1024 * 1024)
            throw std::runtime_error("missing or oversized CMake model input: " + path.string());
        const auto inputPath = fs::absolute(path).string();
        result.inputs.push_back(inputPath);
        std::ifstream input(path, std::ios::binary);
        std::string bytes;
        char buffer[4096];
        while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
            bytes.append(buffer, static_cast<size_t>(input.gcount()));
            if (bytes.size() > 8 * 1024 * 1024) throw std::runtime_error("CMake model exceeds input limit");
        }
        result.inputDigests[inputPath] = topo::dist::sha256Hex(bytes);
        return json::parse(bytes);
    };
    auto objectPath = [&](const std::string& value) {
        // Every model reference is a basename within the selected reply.
        if (fs::path(value).filename() != fs::path(value)) throw std::runtime_error("CMake model reference escapes reply directory");
        return reply / value;
    };
    try {
        std::vector<fs::path> indices;
        for (const auto& entry : fs::directory_iterator(reply)) {
            auto name = entry.path().filename().string();
            if (name.rfind("index-", 0) == 0 && entry.path().extension() == ".json") indices.push_back(entry.path());
        }
        if (indices.empty()) throw std::runtime_error("CMake reply has no index");
        std::sort(indices.begin(), indices.end());
        auto index = read(indices.back());
        json codemodel, cmakeFiles;
        for (const auto& object : index.at("objects")) {
            auto kind = object.value("kind", "");
            if (kind == "codemodel") codemodel = read(objectPath(object.at("jsonFile")));
            if (kind == "cmakeFiles") cmakeFiles = read(objectPath(object.at("jsonFile")));
        }
        if (codemodel.is_null() || cmakeFiles.is_null()) throw std::runtime_error("CMake audit requires both codemodel-v2 and cmakeFiles-v1 replies");
        auto sourceRoot = cppCanonicalPath(codemodel.at("paths").at("source"));
        auto buildRoot = cppCanonicalPath(codemodel.at("paths").at("build"));
        if (sourceRoot != cppCanonicalPath(project)) throw std::runtime_error("CMake source root differs from selected audit project");
        if (codemodel.at("configurations").size() != 1)
            throw std::runtime_error("multiple CMake configurations require a configuration-specific exported model");
        for (const auto& input : cmakeFiles.at("inputs")) {
            const auto path = cppCanonicalPath(input.at("path"), sourceRoot);
            result.inputs.push_back(path);
            if (input.value("isExternal", false) && !input.value("isCMake", false))
                result.dependencies.push_back({"cmake-input:" + path, path, false});
        }
        std::set<std::string> artifacts;
        std::vector<json> targets;
        for (const auto& entry : codemodel.at("configurations").at(0).at("targets")) {
            auto target = read(objectPath(entry.at("jsonFile")));
            for (const auto& artifact : target.value("artifacts", json::array()))
                artifacts.insert(cppCanonicalPath(artifact.at("path"), buildRoot));
            for (const auto& source : target.value("sources", json::array())) {
                auto path = cppCanonicalPath(source.at("path"), sourceRoot);
                result.sources.push_back(path);
                if (fs::is_regular_file(path)) result.inputs.push_back(path);
                else result.unresolved.push_back("CMake source input unavailable: " + path);
            }
            targets.push_back(std::move(target));
        }
        for (const auto& target : targets) {
            if (!target.contains("link")) continue;
            for (const auto& fragment : target.at("link").value("commandFragments", json::array())) {
                if (fragment.value("role", "") != "libraries") continue;
                std::string error;
                auto args = splitCppCommand(fragment.at("fragment"), false, error);
                if (!error.empty()) { result.unresolved.push_back(error); continue; }
                for (const auto& arg : args) {
                    auto path = cppCanonicalPath(arg, buildRoot);
                    if (artifacts.count(path)) continue;
                    if (!arg.empty() && arg.front() != '-' && fs::is_regular_file(path)) {
                        result.dependencies.push_back({"cmake-link:" + path, path, false});
                        result.inputs.push_back(path);
                        result.unresolved.push_back("CMake link artifact ownership needs package/source provenance: " + path);
                    } else {
                        result.unresolved.push_back("CMake link target needs an actual resolved artifact: " + arg);
                    }
                }
            }
        }
    } catch (const std::exception& error) { result.unresolved.push_back(error.what()); }
    return result;
}
}
