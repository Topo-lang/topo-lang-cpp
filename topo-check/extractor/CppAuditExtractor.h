#ifndef TOPO_CPP_AUDIT_EXTRACTOR_H
#define TOPO_CPP_AUDIT_EXTRACTOR_H
#include <nlohmann/json.hpp>
#include <string>
nlohmann::json auditCppCompileModel(const std::string& model, const std::string& project);
nlohmann::json auditCppToolIdentity();
#endif
