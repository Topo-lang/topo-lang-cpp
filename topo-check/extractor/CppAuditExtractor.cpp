#include "CppAuditExtractor.h"
#include "CppAuditDriver.h"
#include "../analysis/CppCompileModel.h"
#include "../analysis/CppCMakeModel.h"
#include "topo/Distribution/Sha256.h"
#include <clang-c/Index.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <link.h>
#include <sys/utsname.h>
#endif

using json = nlohmann::json;

json auditCppToolIdentity() {
    try {
        json identity = {{"schemaVersion", 1}, {"supported", true}, {"images", json::object()}};
#if defined(__APPLE__) || defined(__linux__)
        std::vector<std::string> images;
#ifdef __APPLE__
        // Query only the named OS properties granted by the analysis sandbox.
        // uname also requests properties outside that explicit inventory.
        auto osValue = [](const char* name) {
            char value[256]{}; size_t size = sizeof(value);
            if (sysctlbyname(name, value, &size, nullptr, 0) != 0 || size == 0 || size > sizeof(value))
                throw std::runtime_error(std::string("cannot identify OS property: ") + name);
            return std::string(value);
        };
#if defined(__aarch64__) || defined(__arm64__)
        const auto machine = "arm64";
#elif defined(__x86_64__)
        const auto machine = "x86_64";
#else
        const auto machine = "unknown";
#endif
        identity["os"] = {{"system", osValue("kern.ostype")}, {"release", osValue("kern.osrelease")},
                          {"build", osValue("kern.osversion")}, {"machine", machine}};
        for (uint32_t i = 0; i < _dyld_image_count(); ++i) images.emplace_back(_dyld_get_image_name(i));
#else
        struct utsname os{};
        if (uname(&os) != 0) throw std::runtime_error("cannot identify the running OS");
        identity["os"] = {{"system", os.sysname}, {"release", os.release}, {"version", os.version}, {"machine", os.machine}};
        dl_iterate_phdr([](struct dl_phdr_info* image, size_t, void* data) {
            auto& out = *static_cast<std::vector<std::string>*>(data);
            out.emplace_back(*image->dlpi_name ? image->dlpi_name : "/proc/self/exe");
            return 0;
        }, &images);
#endif
        if (images.empty() || images.size() > 512) throw std::runtime_error("runtime image inventory unavailable or oversized");
        for (const auto& image : images) {
#ifdef __APPLE__
            // Apple's sealed OS images can live only in the dyld shared cache.
            // Their identity is the running OS build above, not a guessed file.
            if (image.rfind("/usr/lib/", 0) == 0 || image.rfind("/System/Library/", 0) == 0 ||
                image.rfind("/System/Cryptexes/OS/", 0) == 0) {
                identity["images"][image] = {{"systemImage", true}};
                continue;
            }
#else
            if (image == "linux-vdso.so.1" || image == "linux-gate.so.1") {
                identity["images"][image] = {{"kernelImage", true}};
                continue;
            }
#endif
            const auto path = std::filesystem::canonical(image);
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 1024ULL * 1024 * 1024)
                throw std::runtime_error("runtime image unavailable or oversized: " + image);
            auto digest = topo::dist::sha256File(path.string());
            if (digest.empty()) throw std::runtime_error("cannot hash runtime image: " + image);
            identity["images"][image] = {{"resolvedPath", path.generic_string()}, {"sha256", digest}};
        }
        auto version = clang_getClangVersion();
        identity["libclang"] = clang_getCString(version);
        clang_disposeString(version);
        return identity;
#else
        throw std::runtime_error("runtime image identity is unavailable on this platform");
#endif
    } catch (const std::exception& error) {
        return {{"supported", false}, {"reason", error.what()}};
    }
}

namespace {
using namespace topo::check;
namespace fs = std::filesystem;
std::string text(CXString str) {
    const auto* ptr = clang_getCString(str);
    std::string value = ptr ? ptr : "";
    clang_disposeString(str);
    return value;
}
std::string qualified(CXCursor cursor) {
    if (clang_Cursor_isNull(cursor)) return "";
    auto parent = clang_getCursorSemanticParent(cursor);
    auto kind = clang_getCursorKind(parent);
    auto name = text(clang_getCursorSpelling(cursor));
    if (kind == CXCursor_TranslationUnit || kind == CXCursor_InvalidFile) return name;
    auto prefix = qualified(parent);
    return prefix.empty() ? name : prefix + "::" + name;
}
std::string usr(CXCursor cursor) { return text(clang_getCursorUSR(clang_getCanonicalCursor(cursor))); }
struct Location { std::string file; unsigned line = 0, column = 0; };
Location location(CXCursor cursor) {
    CXFile file; Location out;
    clang_getExpansionLocation(clang_getCursorLocation(cursor), &file, &out.line, &out.column, nullptr);
    if (file) out.file = cppCanonicalPath(text(clang_getFileName(file)));
    return out;
}
bool within(const std::string& file, const std::string& root) {
    auto p = fs::path(file).lexically_relative(root);
    return !p.empty() && *p.begin() != ".." && !p.is_absolute();
}
bool typeKind(CXCursorKind kind) {
    return kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl ||
           kind == CXCursor_ClassTemplate || kind == CXCursor_ClassTemplatePartialSpecialization;
}
bool functionKind(CXCursorKind kind) {
    return kind == CXCursor_FunctionDecl || kind == CXCursor_FunctionTemplate || kind == CXCursor_CXXMethod ||
           kind == CXCursor_Constructor || kind == CXCursor_Destructor || kind == CXCursor_ConversionFunction;
}
std::string identityId(CXCursor cursor) {
    auto id = usr(cursor);
    auto loc = location(cursor);
    if (id.empty()) id = "location:" + loc.file + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.column);
    return id;
}
struct State {
    json output;
    std::string project;
    std::map<std::string, json> identities;
    std::map<std::string, std::string> digests;
    std::set<std::string> unresolved, inputs;
    size_t nodes = 0;
    bool sawSystem = false;
    CXTranslationUnit tu = nullptr;
    std::string unit;
    bool system(CXCursor cursor) const {
        auto file = location(cursor).file;
        return !file.empty() && !within(file, project) && clang_Location_isInSystemHeader(clang_getCursorLocation(cursor));
    }
    void snapshot(CXFile file) {
        if (!file) return;
        auto path = fs::absolute(text(clang_getFileName(file))).string();
        size_t size = 0;
        auto* data = clang_getFileContents(tu, file, &size);
        if (!data || size > 64 * 1024 * 1024) { unresolved.insert("parsed input bytes unavailable or oversized: " + path); return; }
        auto digest = topo::dist::sha256Hex(data, size);
        auto old = digests.find(path);
        if (old != digests.end() && old->second != digest) unresolved.insert("input changed between translation units: " + path);
        digests[path] = digest; inputs.insert(path);
        if (topo::dist::sha256File(path) != digest) unresolved.insert("input changed during parsing: " + path);
    }
    void recordIdentity(CXCursor cursor, bool declaredInSource = false) {
        if (system(cursor)) { sawSystem = true; return; }
        auto kind = clang_getCursorKind(cursor);
        auto loc = location(cursor);
        if (loc.file.empty()) return;
        const auto id = identityId(cursor);
        auto definition = clang_getCursorDefinition(cursor);
        bool complete = clang_isCursorDefinition(cursor) || !clang_Cursor_isNull(definition);
        if (!clang_Cursor_isNull(definition)) loc = location(definition);
        auto owner = clang_getCursorSemanticParent(cursor);
        auto ownerKind = clang_getCursorKind(owner);
        json item = {{"id", id}, {"name", qualified(cursor)}, {"kind", typeKind(kind) ? "class" : kind == CXCursor_Constructor ? "constructor" : functionKind(kind) ? (ownerKind == CXCursor_Namespace || ownerKind == CXCursor_TranslationUnit ? "function" : "method") : "initializer"},
            {"signature", text(clang_getTypeSpelling(clang_getCursorType(cursor)))},
            {"owner", typeKind(ownerKind) ? identityId(owner) : ""},
            {"file", loc.file}, {"line", loc.line}, {"origin", "libclang"}, {"complete", complete}};
        // Data members are complete ownership facts even without an initializer.
        if (kind == CXCursor_FieldDecl || kind == CXCursor_VarDecl) item["complete"] = true;
        item["declaredInSource"] = declaredInSource;
        item["simpleName"] = text(clang_getCursorSpelling(cursor));
        item["enclosingClass"] = typeKind(ownerKind) ? qualified(owner) : "";
        item["hostKind"] = typeKind(kind) ? 5 : kind == CXCursor_Constructor ? 2 : kind == CXCursor_Destructor ? 3 : ownerKind == CXCursor_Namespace || ownerKind == CXCursor_TranslationUnit ? 0 : 1;
        auto access = clang_getCXXAccessSpecifier(cursor);
        if (access != CX_CXXInvalidAccessSpecifier) item["visibility"] = access == CX_CXXPublic ? 0 : access == CX_CXXProtected ? 1 : 2;
        item["returnType"] = text(clang_getTypeSpelling(clang_getCursorResultType(cursor)));
        item["parameters"] = json::array();
        for (int i = 0; i < clang_Cursor_getNumArguments(cursor); ++i)
            item["parameters"].push_back(text(clang_getTypeSpelling(clang_getCursorType(clang_Cursor_getArgument(cursor, i)))));
        auto found = identities.find(id);
        if (found != identities.end() && found->second.value("complete", false) && item.value("complete", false) && found->second["file"] != item["file"])
            unresolved.insert("one USR resolves to different implementations across compile commands: " + qualified(cursor));
        if (found != identities.end()) item["declaredInSource"] = declaredInSource || found->second.value("declaredInSource", false);
        if (found == identities.end() || (!found->second.value("complete", false) && item.value("complete", false))) identities[id] = std::move(item);
        else found->second["declaredInSource"] = item["declaredInSource"];
        if (!within(loc.file, project)) unresolved.insert("non-system source outside project requires dependency ownership model: " + loc.file);
    }
    void walk(CXCursor cursor, const std::string& caller, CXCursor parent) {
        if (++nodes > 500000) throw std::runtime_error("AST exceeds node limit");
        if (system(cursor)) { sawSystem = true; return; }
        auto kind = clang_getCursorKind(cursor);
        auto current = caller;
        if (typeKind(kind) || functionKind(kind) || kind == CXCursor_FieldDecl || kind == CXCursor_VarDecl) {
            recordIdentity(cursor, true);
            if (functionKind(kind) || ((kind == CXCursor_FieldDecl || kind == CXCursor_VarDecl) && caller.empty())) current = identityId(cursor);
        }
        if (kind == CXCursor_CallExpr) {
            auto ref = clang_getCursorReferenced(cursor);
            auto refKind = clang_getCursorKind(ref);
            auto loc = location(cursor);
            bool direct = !clang_Cursor_isNull(ref) && functionKind(refKind);
            bool dynamic = direct && refKind == CXCursor_CXXMethod && clang_CXXMethod_isVirtual(ref);
            bool dependent = refKind == CXCursor_FunctionTemplate || typeKind(refKind);
            auto target = direct ? identityId(ref) : "";
            bool external = direct && system(ref);
            output["callSites"].push_back({{"caller", current}, {"callee", direct ? qualified(ref) : "<unresolved>"}, {"file", loc.file}, {"line", loc.line}});
            if (direct && !external) recordIdentity(ref);
            if (external) sawSystem = true;
            bool complete = direct && !dynamic && !dependent;
            std::string dispatch = !direct ? "indirect-or-dependent" : dynamic ? "virtual" : dependent ? "template" : "direct";
            output["references"].push_back({{"caller", current}, {"target", target}, {"file", loc.file}, {"line", loc.line},
                {"resolved", direct}, {"dispatch", dispatch}, {"runtimeTargetsComplete", complete},
                {"dependency", external ? "clang-system-headers" : ""}});
            if (current.empty()) unresolved.insert("call has no proven implementation owner: " + loc.file + ":" + std::to_string(loc.line));
            if (clang_getCursorKind(parent) == CXCursor_ParmDecl)
                unresolved.insert("default-argument effect requires caller attribution: " + loc.file + ":" + std::to_string(loc.line));
        }
        if ((kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr) && !current.empty()) {
            auto referenced = clang_getCursorReferenced(cursor);
            auto refKind = clang_getCursorKind(referenced);
            auto refParent = clang_getCursorKind(clang_getCursorSemanticParent(referenced));
            if (refKind == CXCursor_FieldDecl || (refKind == CXCursor_VarDecl &&
                (refParent == CXCursor_TranslationUnit || refParent == CXCursor_Namespace || typeKind(refParent)))) {
                auto loc = location(cursor);
                output["accesses"].push_back({{"caller", current}, {"symbol", qualified(referenced)},
                    {"file", loc.file}, {"line", loc.line}, {"isWrite", true}});
            }
        }
        auto syntaxEffect = [&](const std::string& name) {
            auto loc = location(cursor);
            output["callSites"].push_back({{"caller", current}, {"callee", name}, {"file", loc.file}, {"line", loc.line}, {"escape", true}});
        };
        if (kind == CXCursor_AsmStmt || kind == CXCursor_MSAsmStmt) syntaxEffect("inline-asm");
        if (kind == CXCursor_CXXReinterpretCastExpr) syntaxEffect("reinterpret_cast");
        if (kind == CXCursor_CXXConstCastExpr) syntaxEffect("const_cast");
        if (kind == CXCursor_CStyleCastExpr && clang_getCanonicalType(clang_getCursorType(cursor)).kind == CXType_Pointer) syntaxEffect("c-style-pointer-cast");
        if (kind == CXCursor_GotoStmt || kind == CXCursor_IndirectGotoStmt) syntaxEffect("goto");
        if (clang_isVolatileQualifiedType(clang_getCursorType(cursor))) syntaxEffect("volatile");
        if (kind == CXCursor_UnionDecl && !current.empty()) syntaxEffect("union-in-function");
        if (clang_isAttribute(kind)) syntaxEffect("__attribute__");
        if (kind == CXCursor_CXXNewExpr || kind == CXCursor_CXXDeleteExpr) {
            auto loc = location(cursor);
            output["references"].push_back({{"caller", current}, {"target", ""}, {"file", loc.file}, {"line", loc.line},
                {"resolved", false}, {"dispatch", "implicit-allocation-or-destruction"}, {"runtimeTargetsComplete", false}, {"dependency", ""}});
        }
        if ((kind == CXCursor_VarDecl || kind == CXCursor_ParmDecl) && !caller.empty()) {
            auto type = clang_getCanonicalType(clang_getCursorType(cursor));
            if (type.kind == CXType_Record && !clang_isPODType(type)) {
                auto loc = location(cursor);
                output["references"].push_back({{"caller", current}, {"target", ""}, {"file", loc.file}, {"line", loc.line},
                    {"resolved", false}, {"dispatch", "implicit-destructor"}, {"runtimeTargetsComplete", false}, {"dependency", ""}});
            }
        }
        struct Children { std::vector<CXCursor> values; } children;
        clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData ptr) {
            static_cast<Children*>(ptr)->values.push_back(child); return CXChildVisit_Continue;
        }, &children);
        for (const auto& child : children.values) walk(child, current, cursor);
    }
};
} // namespace

json auditCppCompileModel(const std::string& path, const std::string& project) {
    State state;
    state.project = cppCanonicalPath(project);
    state.output = {{"schemaVersion", 1}, {"provider", "cpp-libclang"}, {"modelPath", fs::absolute(path).string()},
        {"supported", true}, {"complete", false}, {"analysisIsolation", "bounded-process"},
        {"identities", json::array()}, {"references", json::array()}, {"dependencies", json::array()},
        {"callSites", json::array()}, {"accesses", json::array()}};
    auto capture = [&](const std::string& file) {
        state.inputs.insert(file);
        auto value = topo::dist::sha256File(file);
        if (value.empty()) state.unresolved.insert("cannot hash model input: " + file);
        state.digests[file] = value;
    };
    for (const auto* key : {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"})
        if (const auto* value = std::getenv(key); value && *value)
            state.unresolved.insert(std::string("ambient compiler environment must be made explicit in the model: ") + key);
    capture(fs::absolute(path).string());
    const auto before = state.digests;
    auto model = loadCppCompileModel(path, project);
    auto cmake = readCppCMakeModel(fs::path(model.path).parent_path().string(), project);
    for (const auto& input : cmake.inputs) capture(input);
    for (const auto& [file, digest] : cmake.inputDigests) state.digests[file] = digest;
    for (const auto& error : cmake.unresolved) state.unresolved.insert(error);
    for (const auto& dep : cmake.dependencies) state.output["dependencies"].push_back({{"id", dep.id}, {"path", dep.path}, {"version", ""}, {"digest", topo::dist::sha256File(dep.path)}, {"project", dep.project}});
    if (cmake.present && cmake.unresolved.empty()) {
        std::set<std::string> known(cmake.sources.begin(), cmake.sources.end());
        for (const auto& unit : model.units)
            if (!known.count(cppCanonicalPath(unit.file))) state.unresolved.insert("compile command source absent from selected CMake codemodel: " + unit.file);
        std::set<std::string> units;
        for (const auto& unit : model.units) units.insert(cppCanonicalPath(unit.file));
        for (const auto& source : cmake.sources) {
            auto extension = fs::path(source).extension().string();
            if ((extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".c") && !units.count(source))
                state.unresolved.insert("CMake target source has no selected compile command: " + source);
        }
    }
    for (const auto& [file, digest] : model.inputDigests) state.digests[file] = digest;
    for (const auto& error : model.errors) state.unresolved.insert(error);
    CXIndex index = clang_createIndex(0, 0);
    for (const auto& unit : model.units) {
        for (const auto& error : unit.errors) state.unresolved.insert(unit.file + ": " + error);
        for (const auto& file : unit.inputs) if (!state.digests.count(file)) capture(file);
        for (const auto& [file, digest] : unit.inputDigests) state.digests[file] = digest;
        if (!unit.errors.empty()) continue;
        if (const auto rejection = cppAuditDriverRejection(unit); !rejection.empty()) {
            state.unresolved.insert(unit.file + ": " + rejection);
            continue;
        }
        auto args = unit.arguments;
        args.insert(args.begin(), unit.compiler);
        std::vector<const char*> argv;
        for (const auto& arg : args) argv.push_back(arg.c_str());
        const auto parseStatus = clang_parseTranslationUnit2FullArgv(index, unit.file.c_str(), argv.data(), static_cast<int>(argv.size()), nullptr, 0,
                                             CXTranslationUnit_DetailedPreprocessingRecord, &state.tu);
        if (parseStatus != CXError_Success) state.tu = nullptr;
        if (!state.tu) { state.unresolved.insert("cannot parse translation unit: " + unit.file); continue; }
        for (unsigned i = 0; i < clang_getNumDiagnostics(state.tu); ++i) {
            auto diag = clang_getDiagnostic(state.tu, i);
            if (clang_getDiagnosticSeverity(diag) >= CXDiagnostic_Error)
                state.unresolved.insert(unit.file + ": " + text(clang_getDiagnosticSpelling(diag)));
            clang_disposeDiagnostic(diag);
        }
        // Pin every parsed file into unsaved buffers, then build the audited
        // AST from those bytes. A first pass discovers includes; its AST is
        // never the source of accepted facts.
        std::map<std::string, std::string> pinned;
        struct PinContext { CXTranslationUnit tu; std::map<std::string, std::string>* files; size_t bytes = 0; bool exceeded = false; } pin{state.tu, &pinned};
        auto pinFile = [](CXFile file, CXSourceLocation*, unsigned, CXClientData ptr) {
            auto& context = *static_cast<PinContext*>(ptr);
            if (!file || context.exceeded) return;
            size_t size = 0; auto* bytes = clang_getFileContents(context.tu, file, &size);
            if (!bytes || (context.bytes += size) > 128 * 1024 * 1024) { context.exceeded = true; return; }
            (*context.files)[text(clang_getFileName(file))] = std::string(bytes, size);
        };
        pinFile(clang_getFile(state.tu, unit.file.c_str()), nullptr, 0, &pin);
        clang_getInclusions(state.tu, pinFile, &pin);
        std::vector<CXUnsavedFile> unsaved;
        for (const auto& [file, bytes] : pinned) unsaved.push_back({file.c_str(), bytes.c_str(), static_cast<unsigned long>(bytes.size())});
        if (pin.exceeded || clang_reparseTranslationUnit(state.tu, static_cast<unsigned>(unsaved.size()), unsaved.data(), clang_defaultReparseOptions(state.tu)) != 0) {
            state.unresolved.insert("cannot reparse pinned input snapshot: " + unit.file);
            clang_disposeTranslationUnit(state.tu); state.tu = nullptr; continue;
        }
        for (unsigned i = 0; i < clang_getNumDiagnostics(state.tu); ++i) {
            auto diag = clang_getDiagnostic(state.tu, i);
            if (clang_getDiagnosticSeverity(diag) >= CXDiagnostic_Error)
                state.unresolved.insert(unit.file + ": " + text(clang_getDiagnosticSpelling(diag)));
            clang_disposeDiagnostic(diag);
        }
        state.snapshot(clang_getFile(state.tu, unit.file.c_str()));
        struct SnapshotContext { State* state; const std::map<std::string, std::string>* pinned; } snapshot{&state, &pinned};
        clang_getInclusions(state.tu, [](CXFile file, CXSourceLocation*, unsigned, CXClientData ptr) {
            auto& context = *static_cast<SnapshotContext*>(ptr);
            auto name = text(clang_getFileName(file));
            if (!context.pinned->count(name)) context.state->unresolved.insert("new include appeared after snapshot: " + name);
            context.state->snapshot(file);
        }, &snapshot);
        try { state.walk(clang_getTranslationUnitCursor(state.tu), "", clang_getNullCursor()); }
        catch (const std::exception& e) { state.unresolved.insert(e.what()); }
        clang_disposeTranslationUnit(state.tu); state.tu = nullptr;
    }
    clang_disposeIndex(index);
    for (const auto& [path, digest] : state.digests) {
        if (topo::dist::sha256File(path) != digest) state.unresolved.insert("input changed after parsing: " + path);
    }
    for (const auto& [path, digest] : before) {
        if (state.digests[path] != digest) state.unresolved.insert("model changed during parsing: " + path);
    }
    for (const auto& [id, identity] : state.identities) {
        state.output["identities"].push_back(identity);
        if (!identity.value("complete", false)) state.unresolved.insert("definition unavailable: " + identity.value("name", ""));
    }
    if (state.sawSystem) {
        // The aggregate has no single filesystem root. Bind it to the exact
        // parsed input set, including compiler/model and header bytes. Keep
        // those inputs in the audit snapshot so acceptance checks them again.
        const auto identity = topo::dist::sha256Hex(json{{"kind", "clang-system-headers-v1"},
            {"inputs", state.digests}, {"libclang", text(clang_getClangVersion())}}.dump());
        state.output["dependencies"].push_back({{"id", "clang-system-headers"}, {"ownership", "compiler-system-header"},
            {"coordinate", ""}, {"path", ""}, {"version", text(clang_getClangVersion())}, {"digest", identity}, {"project", false}});
    }
    state.output["inputs"] = state.inputs;
    state.output["inputDigests"] = state.digests;
    state.output["buildVariant"] = topo::dist::sha256Hex(json{{"inputs", state.digests}, {"libclang", text(clang_getClangVersion())}}.dump());
    state.output["unresolved"] = state.unresolved;
    state.output["complete"] = state.unresolved.empty() && model.complete();
    return state.output;
}
