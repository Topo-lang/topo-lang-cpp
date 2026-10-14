#ifndef TOPO_CPP_CMAKE_MODEL_H
#define TOPO_CPP_CMAKE_MODEL_H
#include <string>
#include <map>
#include <vector>
namespace topo::check {
struct CppCMakeDependency {
    std::string id, path;
    bool project = false;
};
struct CppCMakeModel {
    bool present = false;
    std::vector<std::string> inputs, sources, unresolved;
    std::vector<CppCMakeDependency> dependencies;
    std::map<std::string, std::string> inputDigests;
};
// Consume an existing File API reply. Never execute CMake configure/build hooks.
CppCMakeModel readCppCMakeModel(const std::string& buildDirectory, const std::string& project);
}
#endif
