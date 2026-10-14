#ifndef TOPO_CPP_COMPILE_MODEL_H
#define TOPO_CPP_COMPILE_MODEL_H

#include <string>
#include <map>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace topo::check {
struct CppCompileUnit {
    std::string file, directory, compiler, variant;
    std::vector<std::string> arguments; // sanitized semantic argv, excludes driver/source/output
    std::vector<std::string> originalArguments;
    std::vector<std::string> inputs; // response files and forced includes
    std::map<std::string, std::string> inputDigests;
    std::vector<std::string> errors;
};
struct CppCompileModel {
    std::string path, project;
    std::vector<CppCompileUnit> units;
    std::vector<std::string> inputs, errors;
    std::map<std::string, std::string> inputDigests;
    bool complete() const;
};
struct CppCompileReadPolicy {
    std::vector<std::string> inputRoots;
    std::vector<std::string> compilerFiles;
    bool fixedEnvironment = false;
};
// Read existing models only. Neither a compiler from the database nor build
// scripts are executed. Preserve every command for a source as a separate TU.
CppCompileModel loadCppCompileModel(const std::string& path, const std::string& project = "",
                                  const CppCompileReadPolicy* readPolicy = nullptr);
// Describe a conservative closure of explicit header-search candidates,
// including absent candidates, quoted object macros and identifier aliases.
// Definitions from every branch/candidate header form a bounded superset;
// unsupported or unresolved preprocessing remains uncached.
nlohmann::json cppAuditInputIdentity(const CppCompileModel& model, const CppCompileReadPolicy* readPolicy = nullptr);
// Exposed to test shell-free tokenization. Windows follows CommandLineToArgvW
// quoting; POSIX accepts quoting/escapes, rejects shell evaluation syntax.
std::vector<std::string> splitCppCommand(const std::string& text, bool windows, std::string& error);
std::string cppCanonicalPath(const std::string& path, const std::string& directory = "");
} // namespace topo::check
#endif
