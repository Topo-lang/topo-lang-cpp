#include "CppAnalysisProvider.h"
#include "CppCompileModel.h"
#include "CppUnsafeCatalog.h"
#include "topo/Distribution/Sha256.h"
#include "topo/Platform/Process.h"
#include "topo/Platform/AnalysisProcess.h"
#include "topo/Platform/SharedLibrary.h"
#include "topo/Platform/ToolResolution.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <array>
#include "CppCallEdgeExtractor.h"
#include "CppCallSiteExtractor.h"
#include "CppImportExtractor.h"
#include "CppSafePatterns.h"
#include "CppSafetyAnalyzer.h"
#include "CppSymbolAccessExtractor.h"
#include "CppSymbolExtractor.h"
#include "ClangdBridge.h"
#include "CppLSPSymbolExtractor.h"
#include "CppLSPCallSiteExtractor.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>

namespace topo::check {

using json = nlohmann::json;
struct CppAnalysisProvider::AuditSnapshot {
    AuditFacts facts;
    json payload;
    std::vector<CheckDiagnostic> diagnostics;
    json execution;
};

namespace {

std::string auditExtractorPath() {
    auto binary = (std::filesystem::path(topo::platform::getExecutableDir()) / "topo-extract-cpp").string();
#ifdef _WIN32
    binary += ".exe";
#endif
    if (!std::filesystem::is_regular_file(binary)) binary = topo::platform::findOnPath("topo-extract-cpp");
    return binary;
}

bool inside(const std::string& path, const std::string& root) {
    const auto relative = std::filesystem::path(cppCanonicalPath(path)).lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
bool nativeTool(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 4> bytes{};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return false;
    return bytes == std::array<unsigned char, 4>{0xcf, 0xfa, 0xed, 0xfe} ||
           bytes == std::array<unsigned char, 4>{0xce, 0xfa, 0xed, 0xfe} ||
           bytes == std::array<unsigned char, 4>{0xfe, 0xed, 0xfa, 0xcf} ||
           bytes == std::array<unsigned char, 4>{0xfe, 0xed, 0xfa, 0xce} ||
           bytes == std::array<unsigned char, 4>{0xca, 0xfe, 0xba, 0xbe} ||
           bytes == std::array<unsigned char, 4>{0xca, 0xfe, 0xba, 0xbf};
}
struct IsolatedCppAudit {
    std::string binary;
    json tool, identity;
    CppCompileReadPolicy reads;
    topo::platform::AnalysisProcessPolicy process;
    CppCompileModel model;
};
json executionToolIdentity(const std::vector<std::filesystem::path>& tools) {
    json identity = json::object();
    for (const auto& tool : tools) {
        const auto path = std::filesystem::canonical(tool);
        const auto digest = topo::dist::sha256File(path.string());
        if (digest.empty()) throw std::runtime_error("cannot bind C++ isolation startup tool: " + path.string());
        identity[path.generic_string()] = digest;
    }
    if (identity.empty()) throw std::runtime_error("C++ isolation startup tools unavailable");
    return identity;
}
void verifyExecutionIdentity(const topo::platform::AnalysisProcessResult& result, const json& tools,
                             const std::string& platformIdentity) {
    if (result.platformIdentity != platformIdentity || executionToolIdentity(result.executionTools) != tools)
        throw std::runtime_error("C++ analysis isolation startup identity changed during execution");
}
IsolatedCppAudit prepareIsolatedCppAudit(const std::string& project, const std::string& modelPath,
                                        const std::vector<std::string>& extraRoots) {
    namespace fs = std::filesystem;
    namespace platform = topo::platform;
    const auto capability = platform::analysisProcessCapability();
    if (!capability.supported) throw std::runtime_error(capability.diagnostic);
    const auto executionTools = executionToolIdentity(capability.executionTools);
    IsolatedCppAudit plan;
    plan.binary = cppCanonicalPath(auditExtractorPath());
    if (!nativeTool(plan.binary) || inside(plan.binary, project))
        throw std::runtime_error("isolated C++ audit requires an installed native extractor outside the subject project");
    plan.reads.inputRoots = {project};
    plan.reads.inputRoots.insert(plan.reads.inputRoots.end(), extraRoots.begin(), extraRoots.end());
    std::sort(plan.reads.inputRoots.begin(), plan.reads.inputRoots.end());
    plan.reads.inputRoots.erase(std::unique(plan.reads.inputRoots.begin(), plan.reads.inputRoots.end()), plan.reads.inputRoots.end());
    plan.reads.fixedEnvironment = true;
    for (const auto& root : plan.reads.inputRoots) plan.process.readOnlyDirectories.push_back(root);
    plan.process.allowedExecutables.push_back(plan.binary);
    plan.process.environment = {{"LANG", "C"}, {"LC_ALL", "C"}};
    // No model or project arguments enter this bootstrap probe. The fixed
    // environment prevents loader/preprocessor injection into the installed
    // native tool. Re-run the same probe under the completed policy below.
    platform::ProcessCaptureLimits limits{45000, 1024 * 1024, 1024 * 1024};
    limits.environment = plan.process.environment;
    const auto bootstrap = platform::runProcessCaptureBounded(plan.binary, {"--audit-cache-identity"}, limits);
    if (bootstrap.exitCode != 0) throw std::runtime_error("installed C++ runtime probe failed: " + bootstrap.diagnostic + bootstrap.stderrOutput);
    plan.tool = json::parse(bootstrap.stdoutOutput);
    if (!plan.tool.value("supported", false) || plan.tool.at("schemaVersion") != 1 || !plan.tool.at("images").is_object())
        throw std::runtime_error("installed C++ runtime probe did not establish an image inventory");
    std::vector<std::string> compilerCandidates = {platform::findOnPath("clang++"), platform::findOnPath("clang"), "/usr/bin/clang++", "/usr/bin/clang"};
    for (const auto& [name, image] : plan.tool.at("images").items()) {
        (void)name;
        if (!image.contains("resolvedPath")) continue;
        const auto path = image.at("resolvedPath").get<std::string>();
        if (inside(path, project)) throw std::runtime_error("installed C++ runtime image is inside the subject project");
        plan.process.toolchainFiles.push_back(path);
        if (fs::path(path).filename().string().find("libclang") != std::string::npos)
            for (const auto* compiler : {"clang++", "clang"})
                compilerCandidates.push_back((fs::path(path).parent_path().parent_path() / "bin" / compiler).string());
    }
    std::vector<std::string> compilerDirectories;
    for (const auto& candidate : compilerCandidates) {
        if (candidate.empty() || !nativeTool(candidate) || inside(candidate, project)) continue;
        const auto path = cppCanonicalPath(candidate);
        if (std::find(plan.reads.compilerFiles.begin(), plan.reads.compilerFiles.end(), path) == plan.reads.compilerFiles.end())
            plan.reads.compilerFiles.push_back(path);
        const auto directory = fs::absolute(candidate).parent_path().string();
        if (std::find(compilerDirectories.begin(), compilerDirectories.end(), directory) == compilerDirectories.end())
            compilerDirectories.push_back(directory);
    }
    std::string fixedPath;
    for (const auto& directory : compilerDirectories) { if (!fixedPath.empty()) fixedPath += ':'; fixedPath += directory; }
    plan.process.environment["PATH"] = fixedPath;
    plan.model = loadCppCompileModel(modelPath, project, &plan.reads);
    if (!plan.model.complete()) {
        std::string reason = "isolated C++ model is unavailable or unauthorized";
        for (const auto& error : plan.model.errors) reason += "; " + error;
        for (const auto& unit : plan.model.units) for (const auto& error : unit.errors) reason += "; " + error;
        throw std::runtime_error(reason);
    }
    for (const auto& unit : plan.model.units) plan.process.readOnlyFiles.push_back(unit.compiler);
    const auto isolated = platform::runAnalysisProcess(plan.binary, {"--audit-cache-identity"}, plan.process, limits);
    if (!isolated.isolationEstablished || isolated.exitCode != 0)
        throw std::runtime_error("isolated C++ runtime probe failed: " + isolated.diagnostic + isolated.stderrOutput);
    verifyExecutionIdentity(isolated, executionTools, capability.platformIdentity);
    const auto isolatedTool = json::parse(isolated.stdoutOutput);
    if (isolatedTool != plan.tool)
        throw std::runtime_error("C++ runtime image inventory changed while establishing isolation: " + json::diff(plan.tool, isolatedTool).dump());
    plan.identity = {{"schemaVersion", 1}, {"policy", "cpp-audit-seatbelt-v1"}, {"backend", isolated.isolation},
                     {"inputRoots", plan.reads.inputRoots}, {"environment", plan.process.environment},
                     {"toolchainFiles", json::array()}, {"executables", {plan.binary}}, {"runtimeIdentity", plan.tool},
                     {"executionTools", executionTools}, {"platformIdentity", capability.platformIdentity}};
    for (const auto& path : plan.process.toolchainFiles) plan.identity["toolchainFiles"].push_back(path.generic_string());
    return plan;
}

void validateAuditPayloadInputs(const json& payload, const CppCompileReadPolicy& reads,
                                const json& execution, const std::string& modelPath) {
    std::set<std::string> trustedFiles(reads.compilerFiles.begin(), reads.compilerFiles.end());
    for (const auto& [name, image] : execution.at("runtimeIdentity").at("images").items()) {
        (void)name;
        if (image.contains("resolvedPath")) trustedFiles.insert(cppCanonicalPath(image.at("resolvedPath").get<std::string>()));
    }
    for (const auto& [path, digest] : execution.at("executionTools").items()) {
        (void)digest; trustedFiles.insert(cppCanonicalPath(path));
    }
    for (const auto& path : execution.at("executables")) trustedFiles.insert(cppCanonicalPath(path.get<std::string>()));
    auto validate = [&](const std::string& path, bool optional = false) {
        if (path.empty() && optional) return;
        if (path.empty() || !std::filesystem::path(path).is_absolute())
            throw std::runtime_error("C++ audit payload input must be an absolute path: " + path);
        const auto physical = cppCanonicalPath(path);
        if (trustedFiles.count(physical)) return;
        for (const auto& root : reads.inputRoots) if (inside(physical, root)) return;
        throw std::runtime_error("C++ audit payload input is outside the project and explicit input roots: " + path);
    };
    const auto reportedModel = payload.at("modelPath").get<std::string>();
    validate(reportedModel);
    if (cppCanonicalPath(reportedModel) != cppCanonicalPath(modelPath))
        throw std::runtime_error("C++ audit payload model differs from the authorized model");
    validate(modelPath);
    // Validate the complete batch before publishing facts or hashing any path.
    for (const auto& path : payload.at("inputs")) validate(path.get<std::string>());
    for (const auto& [path, digest] : payload.at("inputDigests").items()) { (void)digest; validate(path); }
    for (const auto* field : {"identities", "references", "callSites", "accesses"})
        for (const auto& entry : payload.value(field, json::array())) validate(entry.value("file", ""), true);
    for (const auto& entry : payload.at("dependencies")) validate(entry.value("path", ""), true);
}

std::string snapshotName(const json& payload, const std::string& id) {
    for (const auto& identity : payload.value("identities", json::array()))
        if (identity.value("id", "") == id) return identity.value("name", "");
    return id;
}
class AuditSymbolExtractor : public SymbolExtractor {
public:
    explicit AuditSymbolExtractor(json data) : data_(std::move(data)) {}
    std::vector<HostSymbol> extractSymbols(const std::string& file) override {
        std::vector<HostSymbol> out;
        for (const auto& item : data_.value("identities", json::array())) {
            if (item.value("file", "") != cppCanonicalPath(file) || item.value("kind", "") == "initializer" || !item.value("declaredInSource", false)) continue;
            HostSymbol symbol;
            symbol.qualifiedName = item.value("name", ""); symbol.simpleName = item.value("simpleName", "");
            symbol.kind = static_cast<HostSymbolKind>(item.value("hostKind", 0));
            symbol.file = item.value("file", ""); symbol.line = item.value("line", 0);
            symbol.enclosingClass = item.value("enclosingClass", "");
            symbol.returnType = item.value("returnType", "");
            symbol.paramTypes = item.value("parameters", std::vector<std::string>{});
            if (item.contains("visibility")) symbol.hostVisibility = static_cast<Visibility>(item["visibility"].get<int>());
            out.push_back(std::move(symbol));
        }
        return out;
    }
private: json data_;
};
class AuditCallSiteExtractor : public CallSiteExtractor {
public:
    explicit AuditCallSiteExtractor(json data) : data_(std::move(data)) {}
    std::vector<DetectedCallSite> extractCallSites(const std::string& file) override {
        std::vector<DetectedCallSite> out;
        for (const auto& item : data_.value("callSites", json::array())) {
            if (item.value("file", "") != cppCanonicalPath(file)) continue;
            DetectedCallSite site;
            site.callerQualifiedName = snapshotName(data_, item.value("caller", ""));
            site.calleePattern = item.value("callee", "");
            site.file = item.value("file", ""); site.line = item.value("line", 0);
            site.unsafeLevel = item.value("escape", false) ? UnsafeLevel::Escape : CppUnsafeCatalog::classifyCall(site.calleePattern);
            site.capability = classifyApiCall(site.calleePattern);
            if (site.capability && site.unsafeLevel == UnsafeLevel::Safe) site.unsafeLevel = UnsafeLevel::System;
            out.push_back(std::move(site));
        }
        return out;
    }
private: json data_;
};
class AuditEdgeExtractor : public CallEdgeExtractor {
public:
    explicit AuditEdgeExtractor(json data) : data_(std::move(data)) {}
    std::vector<CallEdge> extractCallEdges(const std::string& file) override {
        std::vector<CallEdge> out;
        for (const auto& item : data_.value("references", json::array())) {
            if (item.value("file", "") != cppCanonicalPath(file) || !item.value("resolved", false) || !item.value("dependency", "").empty()) continue;
            out.push_back({snapshotName(data_, item.value("caller", "")), snapshotName(data_, item.value("target", "")), item.value("file", ""), item.value("line", 0)});
        }
        return out;
    }
private: json data_;
};
class AuditAccessExtractor : public SymbolAccessExtractor {
public:
    explicit AuditAccessExtractor(json data) : data_(std::move(data)) {}
    std::vector<SymbolAccess> extractSymbolAccesses(const std::string& file) override {
        std::vector<SymbolAccess> out;
        for (const auto& item : data_.value("accesses", json::array())) {
            if (item.value("file", "") != cppCanonicalPath(file)) continue;
            out.push_back({snapshotName(data_, item.value("caller", "")), item.value("symbol", ""), item.value("isWrite", true), item.value("file", ""), item.value("line", 0)});
        }
        return out;
    }
private: json data_;
};

class CppMergingCallSiteExtractor : public CallSiteExtractor {
public:
    explicit CppMergingCallSiteExtractor(lsp::ClangdBridge& bridge)
        : lspExtractor_(bridge) {}

    std::vector<DetectedCallSite> extractCallSites(const std::string& filePath) override {
        auto callSites = lspExtractor_.extractCallSites(filePath);

        auto regexSites = regexExtractor_.extractCallSites(filePath);
        for (auto& site : regexSites) {
            if (site.unsafeLevel == UnsafeLevel::Escape ||
                site.unsafeLevel == UnsafeLevel::System) {
                bool duplicate = false;
                for (const auto& existing : callSites) {
                    if (existing.callerQualifiedName == site.callerQualifiedName &&
                        existing.calleePattern == site.calleePattern &&
                        existing.file == site.file &&
                        existing.line == site.line) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    callSites.push_back(std::move(site));
                }
            }
        }
        return callSites;
    }

private:
    CppLSPCallSiteExtractor lspExtractor_;
    CppCallSiteExtractor regexExtractor_;
};

} // anonymous namespace

bool CppAnalysisProvider::configureAuditExecution(const AuditExecutionOptions& options, std::string& diagnostic) {
    requireAuditIsolation_ = true;
    expectedAuditExecution_ = nullptr;
    expectedAuditProject_.clear(); expectedAuditModelPath_.clear();
    auditReadPolicy_.reset();
    audit_.reset();
    auditInputRoots_.clear();
    const auto capability = topo::platform::analysisProcessCapability();
    if (!capability.supported) { diagnostic = capability.diagnostic; return false; }
    try {
        for (const auto& root : options.inputRoots) {
            if (!std::filesystem::is_directory(root)) throw std::runtime_error("analysis input root is not an existing directory: " + root);
            auditInputRoots_.push_back(std::filesystem::canonical(root).string());
        }
        diagnostic.clear(); return true;
    } catch (const std::exception& error) { diagnostic = error.what(); return false; }
}

void CppAnalysisProvider::configureAnalysis(const std::string& projectDir, const std::string& projectModelPath) {
    projectDir_ = cppCanonicalPath(projectDir);
    modelPath_ = projectModelPath.empty() ? (std::filesystem::path(projectDir_) / "build/compile_commands.json").string() : (std::filesystem::path(projectModelPath).is_absolute() ? std::filesystem::path(projectModelPath) : std::filesystem::path(projectDir_) / projectModelPath).string();
    audit_.reset();
    if (requireAuditIsolation_) {
        auditReadPolicy_.reset(); expectedAuditExecution_ = nullptr;
        expectedAuditProject_.clear(); expectedAuditModelPath_.clear();
    }
    if (!projectModelPath.empty()) loadAuditSnapshot();
}
void CppAnalysisProvider::loadAuditSnapshot() {
    audit_ = std::make_shared<AuditSnapshot>();
    audit_->facts.provider = "cpp-libclang";
    if (!requireAuditIsolation_) audit_->facts.modelPath = modelPath_;
    audit_->facts.analysisIsolation = requireAuditIsolation_ ? "unestablished" : "bounded-process";
    auto fail = [&](const std::string& message) {
        audit_->facts.complete = false;
        audit_->facts.unresolved.push_back(message);
        CheckDiagnostic diag; diag.severity = Severity::Error; diag.check = "cpp-analysis"; diag.message = message;
        audit_->diagnostics.push_back(std::move(diag));
    };
    auto binary = auditExtractorPath();
    if (binary.empty()) { fail("C++ semantic extractor unavailable; install topo-extract-cpp with libclang support"); return; }
    auto relative = std::filesystem::path(cppCanonicalPath(binary)).lexically_relative(projectDir_);
    if (!relative.empty() && *relative.begin() != ".." && !relative.is_absolute()) {
        fail("audit extractor must be installed outside the subject project"); return;
    }
    topo::platform::BoundedProcessResult result;
    if (requireAuditIsolation_) {
        try {
            auto plan = prepareIsolatedCppAudit(projectDir_, modelPath_, auditInputRoots_);
            auditReadPolicy_ = std::make_shared<CppCompileReadPolicy>(plan.reads);
            audit_->facts.modelPath = modelPath_;
            auto isolated = topo::platform::runAnalysisProcess(plan.binary, {"--audit-model", modelPath_, projectDir_},
                plan.process, {45000, 64 * 1024 * 1024, 1024 * 1024});
            if (!isolated.isolationEstablished) { fail("C++ analysis isolation failed: " + isolated.diagnostic); return; }
            verifyExecutionIdentity(isolated, plan.identity.at("executionTools"), plan.identity.at("platformIdentity"));
            audit_->facts.analysisIsolation = isolated.isolation;
            audit_->execution = plan.identity;
            expectedAuditExecution_ = plan.identity;
            result = std::move(isolated);
        } catch (const std::exception& error) { fail(error.what()); return; }
    } else result = topo::platform::runProcessCaptureBounded(binary, {"--audit-model", modelPath_, projectDir_}, {45000, 64 * 1024 * 1024, 1024 * 1024});
    if (result.exitCode != 0) { fail("C++ semantic extraction failed: " + result.diagnostic + result.stderrOutput); return; }
    if (result.stdoutOutput.size() > 64 * 1024 * 1024) { fail("C++ audit response exceeds size limit"); return; }
    try { consumeAuditPayload(json::parse(result.stdoutOutput), binary); }
    catch (const std::exception& error) { fail(std::string("invalid C++ audit response: ") + error.what()); }
}
void CppAnalysisProvider::consumeAuditPayload(const json& payload, const std::string& binary) {
    auto fail = [&](const std::string& message) {
        audit_->facts.complete = false;
        audit_->facts.unresolved.push_back(message);
        CheckDiagnostic diag; diag.severity = Severity::Error; diag.check = "cpp-analysis"; diag.message = message;
        audit_->diagnostics.push_back(std::move(diag));
    };
    try {
        if (requireAuditIsolation_) {
            if (!auditReadPolicy_ || audit_->execution.is_null()) throw std::runtime_error("C++ audit input policy is unavailable");
            validateAuditPayloadInputs(payload, *auditReadPolicy_, audit_->execution, modelPath_);
        }
        audit_->payload = payload;
        auto& value = audit_->payload;
        auto& facts = audit_->facts;
        facts.schemaVersion = value.at("schemaVersion").get<int>();
        facts.supported = value.at("supported").get<bool>(); facts.complete = value.at("complete").get<bool>();
        facts.buildVariant = value.at("buildVariant").get<std::string>();
        facts.inputs = value.at("inputs").get<std::vector<std::string>>();
        facts.inputDigests = value.at("inputDigests").get<std::map<std::string, std::string>>();
        facts.unresolved = value.at("unresolved").get<std::vector<std::string>>();
        for (const auto& message : facts.unresolved) {
            CheckDiagnostic diag; diag.severity = Severity::Error; diag.check = "cpp-analysis"; diag.message = message;
            audit_->diagnostics.push_back(std::move(diag));
        }
        for (const auto& i : value.at("identities")) facts.identities.push_back({i.at("id"), i.at("name"), i.at("kind"), i.at("signature"), i.at("owner"), i.at("file"), i.at("line"), i.at("origin"), i.at("complete")});
        for (const auto& r : value.at("references")) facts.references.push_back({r.at("caller"), r.at("target"), r.at("file"), r.at("line"), r.at("resolved"), r.at("dispatch"), r.at("runtimeTargetsComplete"), r.at("dependency")});
        for (const auto& d : value.at("dependencies")) facts.dependencies.push_back({d.at("id"), d.at("path"), d.at("version"), d.at("digest"), d.at("project"), d.value("ownership", ""), d.value("coordinate", "")});
        for (const auto& [file, digest] : facts.inputDigests)
            if (topo::dist::sha256File(file) != digest) fail("analysis input changed before provider consumption: " + file);
        facts.inputs.push_back(binary); facts.inputDigests[binary] = topo::dist::sha256File(binary);
        if (requireAuditIsolation_ && !audit_->execution.is_null()) {
            facts.buildVariant = topo::dist::sha256Hex(json{{"variant", facts.buildVariant}, {"execution", audit_->execution}}.dump());
            for (const auto& [name, image] : audit_->execution.at("runtimeIdentity").at("images").items()) {
                (void)name;
                if (!image.contains("resolvedPath")) continue;
                const auto path = image.at("resolvedPath").get<std::string>();
                const auto digest = image.at("sha256").get<std::string>();
                if (topo::dist::sha256File(path) != digest) fail("C++ runtime input changed before provider consumption: " + path);
                facts.inputs.push_back(path); facts.inputDigests[path] = digest;
            }
            for (const auto& [path, digest] : audit_->execution.at("executionTools").items()) {
                if (topo::dist::sha256File(path) != digest.get<std::string>()) fail("C++ isolation startup tool changed before provider consumption: " + path);
                facts.inputs.push_back(path); facts.inputDigests[path] = digest.get<std::string>();
            }
        }
        if (!audit_->diagnostics.empty()) facts.complete = false;
    } catch (const std::exception& error) { fail(std::string("invalid C++ audit response: ") + error.what()); }
}

json CppAnalysisProvider::auditCacheIdentity(const std::string& project, const std::string& modelPath) const {
    namespace fs = std::filesystem;
    auto bypass = [](const std::string& reason) { return json{{"supported", false}, {"reason", reason}}; };
    if (requireAuditIsolation_) {
        expectedAuditExecution_ = nullptr;
        expectedAuditProject_.clear(); expectedAuditModelPath_.clear();
        auditReadPolicy_.reset();
    }
    try {
        if (modelPath.empty()) return bypass("cache requires an explicit discovered compilation database");
        const auto root = cppCanonicalPath(project);
        const auto path = cppCanonicalPath(modelPath, root);
        std::optional<IsolatedCppAudit> isolatedPlan;
        if (requireAuditIsolation_) {
            isolatedPlan = prepareIsolatedCppAudit(root, path, auditInputRoots_);
            expectedAuditExecution_ = isolatedPlan->identity;
            expectedAuditProject_ = root; expectedAuditModelPath_ = path;
            auditReadPolicy_ = std::make_shared<CppCompileReadPolicy>(isolatedPlan->reads);
        }
        const auto model = isolatedPlan ? isolatedPlan->model : loadCppCompileModel(path, root);
        if (!model.complete() || model.units.empty()) return bypass("compilation model is incomplete");
        if (fs::exists(fs::path(path).parent_path() / ".cmake/api/v1/reply") || fs::exists(fs::path(root) / "CMakeLists.txt"))
            return bypass("CMake discovery is outside the closed-input cache subset");
        auto closure = cppAuditInputIdentity(model, isolatedPlan ? &isolatedPlan->reads : nullptr);
        if (!closure.value("supported", false)) return bypass(closure.value("reason", "header search closure unavailable"));
        json inputs = closure.at("inputs");
        closure.erase("inputs");
        auto capture = [&](const std::string& file) {
            if (!fs::is_regular_file(file) || fs::file_size(file) > 1024ULL * 1024 * 1024)
                throw std::runtime_error("cache input unavailable or oversized: " + file);
            auto digest = topo::dist::sha256File(file);
            if (digest.empty()) throw std::runtime_error("cannot hash cache input: " + file);
            inputs[fs::absolute(file).generic_string()] = {{"resolvedPath", fs::canonical(file).generic_string()}, {"sha256", digest}};
        };
        const auto binary = auditExtractorPath();
        if (binary.empty()) return bypass("C++ extractor unavailable");
        auto relative = fs::path(cppCanonicalPath(binary)).lexically_relative(root);
        if (!relative.empty() && *relative.begin() != ".." && !relative.is_absolute()) return bypass("cache extractor is inside the subject project");
        capture(binary);
        json tool;
        if (isolatedPlan) tool = isolatedPlan->tool;
        else {
            auto result = topo::platform::runProcessCaptureBounded(binary, {"--audit-cache-identity"}, {45000, 1024 * 1024, 1024 * 1024});
            if (result.exitCode != 0) return bypass("runtime image probe failed: " + result.diagnostic + result.stderrOutput);
            tool = json::parse(result.stdoutOutput);
        }
        if (!tool.value("supported", false)) return bypass("runtime image inventory unavailable: " + tool.value("reason", "unknown"));
        if (tool.at("schemaVersion") != 1 || !tool.at("images").is_object() || tool.at("images").empty())
            return bypass("runtime image identity schema mismatch");
        for (const auto& [name, image] : tool.at("images").items()) {
            (void)name;
            if (!image.contains("resolvedPath")) continue;
            const auto imagePath = image.at("resolvedPath").get<std::string>();
            auto child = fs::path(imagePath).lexically_relative(root);
            if (!child.empty() && *child.begin() != ".." && !child.is_absolute()) return bypass("runtime image is inside the subject project");
            // The installed probe just hashed these loaded images. Reuse that
            // immutable observation here; report construction checks its bytes
            // and the final identity probe repeats the complete observation.
            if (inputs.contains(imagePath) && inputs.at(imagePath).at("sha256") != image.at("sha256"))
                return bypass("runtime image changed during identity capture");
            inputs[imagePath] = image;
        }
        if (isolatedPlan) for (const auto& [path, digest] : isolatedPlan->identity.at("executionTools").items()) {
            capture(path);
            if (inputs.at(path).at("sha256") != digest) return bypass("isolation startup tool changed during cache identity capture");
        }
        return {{"supported", true}, {"schemaVersion", 1}, {"provider", "cpp-libclang-explicit-header-closure-v2"},
                {"project", root}, {"modelPath", path}, {"inputs", inputs}, {"searchClosure", closure}, {"tool", tool},
                {"execution", isolatedPlan ? isolatedPlan->identity : json(nullptr)}};
    } catch (const std::exception& error) { return bypass(error.what()); }
}
json CppAnalysisProvider::exportAuditCache() const {
    if (!audit_ || !audit_->facts.supported || !audit_->facts.complete || !audit_->facts.unresolved.empty() || !audit_->diagnostics.empty()) return nullptr;
    return {{"schemaVersion", 1}, {"project", projectDir_}, {"modelPath", modelPath_},
            {"payload", audit_->payload}, {"diagnostics", json::array()}, {"execution", audit_->execution}};
}
bool CppAnalysisProvider::importAuditCache(const json& data) {
    audit_.reset();
    try {
        if (data.at("schemaVersion") != 1 || !data.at("diagnostics").is_array() || !data.at("diagnostics").empty()) return false;
        if (requireAuditIsolation_) {
            if (!auditReadPolicy_ || expectedAuditExecution_.is_null() ||
                data.value("execution", json(nullptr)) != expectedAuditExecution_ ||
                !std::filesystem::path(data.at("project").get<std::string>()).is_absolute() ||
                !std::filesystem::path(data.at("modelPath").get<std::string>()).is_absolute() ||
                cppCanonicalPath(data.at("project").get<std::string>()) != expectedAuditProject_ ||
                cppCanonicalPath(data.at("modelPath").get<std::string>()) != expectedAuditModelPath_) return false;
            validateAuditPayloadInputs(data.at("payload"), *auditReadPolicy_, expectedAuditExecution_, expectedAuditModelPath_);
        }
        projectDir_ = data.at("project").get<std::string>(); modelPath_ = data.at("modelPath").get<std::string>();
        audit_ = std::make_shared<AuditSnapshot>();
        audit_->facts.provider = "cpp-libclang"; audit_->facts.modelPath = modelPath_;
        audit_->facts.analysisIsolation = requireAuditIsolation_ ? expectedAuditExecution_.at("backend").get<std::string>() : "bounded-process";
        audit_->execution = data.value("execution", json(nullptr));
        consumeAuditPayload(data.at("payload"), auditExtractorPath());
        if (audit_->facts.schemaVersion == 1 && audit_->facts.supported && audit_->facts.complete &&
            audit_->facts.unresolved.empty() && audit_->diagnostics.empty()) return true;
    } catch (const std::exception&) {}
    audit_.reset();
    return false;
}
AuditFacts CppAnalysisProvider::auditFacts(const std::vector<std::string>&) const {
    if (audit_) return audit_->facts;
    AuditFacts result;
    result.provider = "cpp-libclang";
    result.unresolved.push_back("C++ audit requires an existing compile_commands.json; provide --project-model or configure the build with CMAKE_EXPORT_COMPILE_COMMANDS=ON");
    return result;
}
std::vector<CheckDiagnostic> CppAnalysisProvider::analysisDiagnostics() const {
    return audit_ ? audit_->diagnostics : std::vector<CheckDiagnostic>{};
}

CppAnalysisProvider::~CppAnalysisProvider() {
    shutdownLSP();
}

bool CppAnalysisProvider::initLSP(const std::string& projectDir, bool /*verbose*/) {
    namespace fs = std::filesystem;
    if (bridge_ && bridge_->isAvailable()) return true;

    auto bridge = std::make_unique<lsp::ClangdBridge>();
    std::string buildDir = (fs::path(projectDir) / "build").string();
    std::string rootUri = "file://" + fs::canonical(projectDir).string();

    if (!bridge->start("", buildDir, rootUri)) {
        return false;
    }

    if (!bridge->isAvailable()) {
        bridge->stop();
        return false;
    }

    if (!bridge->waitForIndex(std::chrono::milliseconds{30000})) {
        std::cerr << "[topo-lsp] clangd index not ready after 30s, L2 analysis may be incomplete\n";
    }

    bridge_ = std::move(bridge);
    return true;
}

void CppAnalysisProvider::shutdownLSP() {
    if (bridge_) {
        bridge_->stop();
        bridge_.reset();
    }
}

bool CppAnalysisProvider::isLSPReady() const {
    return bridge_ && bridge_->isAvailable();
}

std::unique_ptr<SymbolExtractor> CppAnalysisProvider::createSymbolExtractor() {
    if (audit_) return std::make_unique<AuditSymbolExtractor>(audit_->payload);
    if (isLSPReady()) {
        return std::make_unique<CppLSPSymbolExtractor>(*bridge_);
    }
    return std::make_unique<CppSymbolExtractor>();
}

std::unique_ptr<ImportExtractor> CppAnalysisProvider::createImportExtractor() {
    return std::make_unique<CppImportExtractor>();
}

std::unique_ptr<CallSiteExtractor> CppAnalysisProvider::createCallSiteExtractor() {
    if (audit_) return std::make_unique<AuditCallSiteExtractor>(audit_->payload);
    if (isLSPReady()) {
        return std::make_unique<CppMergingCallSiteExtractor>(*bridge_);
    }
    return std::make_unique<CppCallSiteExtractor>();
}

std::unique_ptr<CallEdgeExtractor> CppAnalysisProvider::createCallEdgeExtractor() {
    if (audit_) return std::make_unique<AuditEdgeExtractor>(audit_->payload);
    // L1-only — no LSP/L2 merging layer.
    return std::make_unique<CppCallEdgeExtractor>();
}

std::unique_ptr<SymbolAccessExtractor> CppAnalysisProvider::createSymbolAccessExtractor() {
    if (audit_) return std::make_unique<AuditAccessExtractor>(audit_->payload);
    return std::make_unique<CppSymbolAccessExtractor>();
}

std::vector<std::string> CppAnalysisProvider::collectSourceFiles(
    const std::string& projectDir,
    const std::vector<std::string>& includeDirs) const {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    // Audit failures must never select the ordinary unconfined discovery path.
    if (requireAuditIsolation_ && (!audit_ || !auditReadPolicy_)) return files;
    if (audit_) {
        std::set<std::string> selected;
        auto model = loadCppCompileModel(modelPath_, projectDir_, requireAuditIsolation_ ? auditReadPolicy_.get() : nullptr);
        if (requireAuditIsolation_ && !model.complete()) return files;
        for (const auto& identity : audit_->facts.identities) selected.insert(identity.file);
        for (const auto& unit : model.units) if (!unit.file.empty() && (!requireAuditIsolation_ || unit.errors.empty())) selected.insert(unit.file);
        return {selected.begin(), selected.end()};
    }
    std::vector<fs::path> searchDirs = {
        fs::path(projectDir) / "src",
        fs::path(projectDir)};
    for (const auto& incDir : includeDirs) {
        searchDirs.push_back(fs::path(incDir));
    }
    std::set<std::string> seen;
    auto addIfSource = [&](const fs::path& p) {
        auto ext = p.extension().string();
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
            ext == ".h" || ext == ".hpp" || ext == ".hxx") {
            std::string path = p.string();
            if (seen.insert(path).second) {
                files.push_back(path);
            }
        }
    };
    for (const auto& dir : searchDirs) {
        std::error_code ec;
        // [build].sources entries are commonly plain files ("main.cpp") and
        // arrive here verbatim; analyze them as single TUs. Feeding a file to
        // recursive_directory_iterator throws ("Not a directory") and aborted
        // the checker before this guard.
        if (fs::is_regular_file(dir, ec)) {
            addIfSource(dir);
            continue;
        }
        if (!fs::exists(dir, ec)) continue;
        // Non-throwing iteration (error_code construction + increment, same
        // pattern as CheckRunner::discoverRelevantFiles and the Rust
        // provider): an unreadable or vanishing entry must degrade to
        // skipping it, never abort the process.
        fs::recursive_directory_iterator it(dir, ec);
        if (ec) continue;
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            addIfSource(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::optional<CheckResult> CppAnalysisProvider::runDeepContainment(
    const SymbolTable& symbols,
    const std::vector<std::string>& sourceFiles,
    const ContainmentConfig& config,
    const std::string& projectDir,
    bool verbose) {
    namespace fs = std::filesystem;
    CheckResult result;

    CppSafePatterns patterns;
    if (!patterns.loadDefault()) {
        CheckDiagnostic d;
        d.severity = Severity::Warning;
        d.check = "containment-l2";
        d.message = "CppSafePatterns.toml not found — cannot run L2 analysis";
        result.addDiagnostic(std::move(d));
        return result;
    }

    if (!isLSPReady()) {
        initLSP(projectDir, verbose);
    }
    if (!isLSPReady()) {
        CheckDiagnostic d;
        d.severity = Severity::Warning;
        d.check = "containment-l2";
        d.message = "clangd unavailable — falling back to L1";
        result.addDiagnostic(std::move(d));
        return result;
    }

    if (verbose) {
        std::cerr << "L2 containment: clangd started, analyzing...\n";
    }

    std::string buildDir = (fs::path(projectDir) / "build").string();
    CppSafetyAnalyzer analyzer(*bridge_, patterns);
    analyzer.setBuildDir(buildDir);
    result = analyzer.analyze(symbols, sourceFiles, config);

    return result;
}

std::unique_ptr<LanguageAnalysisProvider> createCppAnalysisProvider() {
    return std::unique_ptr<LanguageAnalysisProvider>(new CppAnalysisProvider());
}

} // namespace topo::check
