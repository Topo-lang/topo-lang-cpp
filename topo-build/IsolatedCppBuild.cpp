#include "IsolatedCppBuild.h"
#include "CppDriver.h"
#include "topo/Build/ArtifactEvidence.h"
#include "topo/Distribution/Sha256.h"
#include "topo/Platform/Process.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>

namespace topo::build {
namespace fs = std::filesystem;
namespace ev = evidence;
namespace {
constexpr const char* Target = "arm64-apple-macosx15.0.0";

#if defined(__APPLE__) && defined(__aarch64__)
bool writeText(const fs::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary); file << data; file.close(); return !!file;
}
std::string readText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
std::string quoteProfile(const fs::path& path) {
    std::string out = "\"";
    for (char c : path.generic_string()) {
        if (c == '\\' || c == '"') out += '\\';
        if (static_cast<unsigned char>(c) < 32) return {};
        out += c;
    }
    return out + "\"";
}
bool scalar(const std::string& type, bool allowVoid = false) {
    return (allowVoid && type == "void") || type == "int" || type == "long long" ||
           type == "float" || type == "double";
}
bool toolLibraries(const fs::path& compiler, const fs::path& linker,
                   std::map<std::string, std::string>& identities, std::string& error) {
    std::vector<fs::path> pending{compiler, linker};
    for (size_t i=0; i<pending.size(); ++i) {
        if (pending.size()>128) { error="toolchain library closure exceeds limit"; return false; }
        std::error_code ec; const auto image=fs::canonical(pending[i],ec);
        if (ec) { error="cannot resolve toolchain image"; return false; }
        if (identities.count(image.string())) continue;
        const auto digest=dist::sha256File(image.string());
        if (!ev::validDigest(digest)) { error="cannot hash toolchain image"; return false; }
        identities[image.string()]=digest;
        const auto listing=platform::runProcessCaptureWithTimeout("/usr/bin/otool",{"-L",image.string()},30000);
        if (listing.exitCode!=0) { error="cannot inspect toolchain dynamic libraries"; return false; }
        std::istringstream lines(listing.stdoutOutput);std::string line;
        while(std::getline(lines,line)) {
            const auto marker=line.find(" (compatibility version");if(marker==std::string::npos)continue;
            const auto start=line.find_first_not_of(" \t");if(start==std::string::npos)continue;
            const auto name=line.substr(start,marker-start);
            if(name.rfind("/usr/lib/",0)==0||name.rfind("/System/Library/",0)==0)continue;
            fs::path candidate;
            if(name.rfind("@rpath/",0)==0) {
                for(const auto& base:{image.parent_path(),compiler.parent_path().parent_path()/"lib",linker.parent_path().parent_path()/"lib"})
                    if(fs::is_regular_file(base/name.substr(7),ec)){candidate=base/name.substr(7);break;}
            } else if(name.rfind("@loader_path/",0)==0)candidate=image.parent_path()/name.substr(13);
            else if(fs::path(name).is_absolute())candidate=name;
            if(candidate.empty()){error="unsupported toolchain library location: "+name;return false;}
            candidate=fs::canonical(candidate,ec);
            if(ec){error="missing toolchain library: "+name;return false;}
            if(!identities.count(candidate.string())&&std::find(pending.begin(),pending.end(),candidate)==pending.end())pending.push_back(candidate);
        }
    }
    return true;
}
bool exportedSignatures(const nlohmann::json& ast, nlohmann::json& exports, std::string& error) {
    exports = nlohmann::json::array();
    std::set<std::string> names;
    // Restrict bodies to scalar expressions and control flow. Calls, globals,
    // constructors, inline assembly, attributes and C++ object features require
    // a broader native-ABI/import model and are deliberately rejected here.
    const std::set<std::string> bodyKinds = {"CompoundStmt", "ReturnStmt", "BinaryOperator", "UnaryOperator",
        "ImplicitCastExpr", "CStyleCastExpr", "CXXStaticCastExpr", "ParenExpr", "IntegerLiteral", "FloatingLiteral",
        "DeclRefExpr", "DeclStmt", "VarDecl", "IfStmt", "ConditionalOperator", "NullStmt", "ParmVarDecl",
        "ForStmt", "WhileStmt", "DoStmt", "BreakStmt", "ContinueStmt", "CompoundAssignOperator", "CXXBoolLiteralExpr"};
    std::function<bool(const nlohmann::json&, size_t)> body = [&](const auto& node, size_t depth) {
        if (!node.is_object() || depth > 256) return false;
        const auto kind = node.value("kind", std::string());
        if (!bodyKinds.count(kind)) return false;
        if (node.contains("type")) {
            auto type = node.at("type").value("qualType", std::string());
            if (!scalar(type, true) && type != "bool") return false;
        }
        if (kind == "VarDecl" && (node.contains("storageClass") || node.contains("tls"))) return false;
        if (kind == "DeclRefExpr") {
            if (!node.contains("referencedDecl")) return false;
            const auto ref = node.at("referencedDecl").value("kind", std::string());
            if (ref != "ParmVarDecl" && ref != "VarDecl") return false;
        }
        if (node.contains("inner")) for (const auto& child : node.at("inner")) if (!body(child, depth + 1)) return false;
        return true;
    };
    std::function<bool(const nlohmann::json&, bool)> declaration = [&](const auto& node, bool cLinkage) {
        if (!node.is_object()) return false;
        if (node.value("isImplicit", false)) return true;
        const auto kind = node.value("kind", std::string());
        if (kind == "TranslationUnitDecl" || kind == "LinkageSpecDecl") {
            if (kind == "LinkageSpecDecl" && node.value("language", std::string()) != "C") return false;
            if (node.contains("inner")) for (const auto& child : node.at("inner"))
                if (!declaration(child, cLinkage || kind == "LinkageSpecDecl")) return false;
            return true;
        }
        if (kind != "FunctionDecl" || !cLinkage || node.contains("storageClass") ||
            node.value("variadic", false) || node.value("inline", false)) return false;
        const auto name = node.value("name", std::string());
        if (name.empty() || name.rfind("__", 0) == 0 || !names.insert(name).second) return false;
        if (node.value("mangledName", std::string()) != "_" + name) return false;
        const auto signature = node.at("type").value("qualType", std::string());
        const auto split = signature.find(" (");
        if (split == std::string::npos || !scalar(signature.substr(0, split), true)) return false;
        std::vector<std::string> params;
        bool definition = false;
        if (node.contains("inner")) for (const auto& child : node.at("inner")) {
            if (child.value("kind", std::string()) == "ParmVarDecl") {
                auto type = child.at("type").value("qualType", std::string());
                if (!scalar(type) || child.contains("init")) return false;
                params.push_back(type);
            } else if (child.value("kind", std::string()) == "CompoundStmt" && !definition) {
                definition = true; if (!body(child, 0)) return false;
            } else return false;
        }
        std::string expected = signature.substr(0, split) + " (";
        for (size_t i = 0; i < params.size(); ++i) { if (i) expected += ", "; expected += params[i]; }
        expected += ")";
        if (!definition || signature != expected || params.size() > 16) return false;
        exports.push_back({{"name", name}, {"abi", "c"}, {"signature", signature},
                           {"returnType",signature.substr(0,split)}, {"parameterTypes",params}});
        return true;
    };
    try {
        if (!declaration(ast, false) || exports.empty()) {
            error = "isolated-build supports only defined extern C scalar functions with scalar bodies"; return false;
        }
        std::sort(exports.begin(), exports.end(), [](const auto& a, const auto& b) { return a.at("name") < b.at("name"); });
        return true;
    } catch (const std::exception&) { error = "invalid compiler AST output"; return false; }
}

// Inspect the actual little-endian Mach-O image without executing it or loading
// a library. This limited reader accepts one ARM64 dylib and a conventional
// nlist symbol table, and requires its external definitions to equal the AST.
bool inspectDylib(const std::string& bytes, const nlohmann::json& exports, std::string& error) {
    auto u32 = [&](size_t pos) -> uint32_t {
        if (pos > bytes.size() || bytes.size() - pos < 4) throw std::runtime_error("truncated Mach-O");
        uint32_t value = 0; for (size_t i = 0; i < 4; ++i) value |= uint32_t(static_cast<unsigned char>(bytes[pos+i])) << (8*i);
        return value;
    };
    auto stringAt = [&](size_t pos, size_t end) {
        if (pos >= end || end > bytes.size()) throw std::runtime_error("invalid Mach-O string");
        auto stop = bytes.find('\0', pos); if (stop == std::string::npos || stop >= end) throw std::runtime_error("unterminated Mach-O string");
        return bytes.substr(pos, stop-pos);
    };
    try {
        if (bytes.size()<32 || u32(0) != 0xfeedfacf || u32(4) != 0x0100000c || u32(12) != 6) throw std::runtime_error("expected ARM64 Mach-O dylib");
        const auto count = u32(16), commandBytes = u32(20);
        if (count > 4096 || commandBytes > bytes.size() - 32) throw std::runtime_error("invalid Mach-O commands");
        size_t cursor=32, table=0, strings=0, stringSize=0, symbols=0;
        std::set<std::string> dependencies;
        for (size_t i=0;i<count;++i) {
            const auto command=u32(cursor), size=u32(cursor+4);
            if (size < 8 || size > 32+commandBytes-cursor) throw std::runtime_error("invalid Mach-O command size");
            if (command == 2) { if (size < 24 || table) throw std::runtime_error("invalid symbol table"); table=u32(cursor+8);symbols=u32(cursor+12);strings=u32(cursor+16);stringSize=u32(cursor+20); }
            if (command==0xc || command==0x80000018 || command==0x8000001f || command==0x80000023) {
                if (size < 24) throw std::runtime_error("invalid dylib import");
                dependencies.insert(stringAt(cursor+u32(cursor+8),cursor+size));
            }
            if (command == 0x8000001c || command == 0x27 || command == 0x80000028)
                throw std::runtime_error("runtime path or dyld environment outside isolated profile");
            cursor += size;
        }
        if (cursor != 32+commandBytes || dependencies != std::set<std::string>{"/usr/lib/libSystem.B.dylib"})
            throw std::runtime_error("unexpected native runtime dependency");
        if (!table || table > bytes.size() || symbols > (bytes.size()-table)/16 ||
            strings > bytes.size() || stringSize > bytes.size()-strings) throw std::runtime_error("invalid Mach-O symbol ranges");
        std::set<std::string> actual, expected;
        for (const auto& item : exports) expected.insert("_" + item.at("name").get<std::string>());
        for (size_t i=0;i<symbols;++i) {
            const size_t offset=table+i*16; const unsigned char type=bytes[offset+4];
            if (type&0xe0) continue; // debug records
            if (!(type&1)) continue;
            const auto nameIndex=u32(offset);
            if (nameIndex >= stringSize) throw std::runtime_error("invalid symbol name offset");
            auto name=stringAt(strings+nameIndex,strings+stringSize);
            if ((type&0xe)==0) throw std::runtime_error("undefined native symbol: " + name);
            if ((type&0xe)!=0xe || (type&0x10)) throw std::runtime_error("unsupported external symbol kind");
            actual.insert(name);
        }
        if (actual != expected) throw std::runtime_error("binary exports differ from compiler AST signatures");
        return true;
    } catch (const std::exception& e) { error=e.what(); return false; }
}
#endif
} // namespace

std::vector<std::string> isolatedCppCompilePrefix(const BuildConfig& cfg,std::string& error) {
    std::error_code ec;
    const auto sdk=fs::canonical("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",ec);
    if(ec){error="isolated-build requires the macOS CommandLineTools SDK";return {};}
    return {"--no-default-config",std::string("--target=")+Target,"-std=c++17","-x","c++",
        "-nostdinc","-nostdinc++","-isysroot",sdk.string(),"-fno-autolink","-fno-exceptions","-fno-rtti",
        "-fPIC","-O"+std::to_string(static_cast<int>(cfg.optLevel))};
}

DriverResult buildIsolatedCpp(const BuildConfig& cfg, std::string& error) {
    DriverResult result; result.exitCode = 1;
#if !defined(__APPLE__) || !defined(__aarch64__)
    (void)cfg; error = "isolated-build currently requires macOS ARM64"; return result;
#else
    try {
        if (cfg.language != HostLanguage::Cpp || cfg.sources.size() != 1 || cfg.outputType != OutputType::Shared ||
            !cfg.cppFlags.empty() || !cfg.includeDirs.empty() || !cfg.linkDirs.empty() || !cfg.linkLibs.empty() ||
            cfg.standard != "c++17" || cfg.embedIR || cfg.buildMode != BuildMode::Dev || cfg.evidenceOutput.empty()) {
            error = "isolated-build requires one C++17 source, shared output, and empty project flags/includes/link dependencies"; return result;
        }
        for(auto mode:{cfg.parallelCfg.mode,cfg.adaptiveCfg.mode,cfg.dataLayoutCfg.mode,cfg.indirectionCfg.mode,
            cfg.observabilityCfg.mode,cfg.lifetimeCfg.mode,cfg.loopParallelCfg.mode,cfg.prefetchCfg.mode,
            cfg.typeNarrowingCfg.mode,cfg.containmentCfg.mode})if(mode!=FeatureMode::Off){
                error="isolated-build does not support Topo optimization transforms";return result;
            }
        if(cfg.pipelineCfg.mode==FeatureMode::Force || cfg.obfMode!=ObfuscationMode::Normal || !cfg.obfSalt.empty() ||
            cfg.dumpIR || cfg.dumpMap || cfg.debugInternal || static_cast<int>(cfg.optLevel)<0 || static_cast<int>(cfg.optLevel)>3){
            error="isolated-build does not support the requested transform/debug configuration";return result;
        }
        std::error_code ec;
        const auto compiler=fs::canonical(cfg.hostCompilerPath,ec); if (ec) {error="cannot resolve pinned compiler";return result;}
        const auto linker=fs::canonical(cfg.evidenceLinkerPath,ec); if (ec) {error="cannot resolve pinned linker";return result;}
        if (!fs::path(cfg.hostCompilerPath).is_absolute() || !fs::path(cfg.evidenceLinkerPath).is_absolute() ||
            !ev::validDigest(cfg.evidenceCompilerSha256) || !ev::validDigest(cfg.evidenceLinkerSha256) ||
            dist::sha256File(compiler.string()) != cfg.evidenceCompilerSha256 ||
            dist::sha256File(linker.string()) != cfg.evidenceLinkerSha256) {
            error="isolated-build requires matching externally trusted compiler and linker SHA-256 pins"; return result;
        }
        const fs::path sdk = fs::canonical("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",ec);
        if (ec) {error="isolated-build requires the macOS CommandLineTools SDK";return result;}
        const fs::path source=fs::canonical(cfg.sources.front(),ec);
        if (ec || !fs::is_regular_file(source) || fs::file_size(source)>65536) {error="isolated source must be a regular file up to 64 KiB";return result;}
        if(!cfg.checkModelPath.empty() && !validateCppCheckModel(cfg,fs::path(),error))return result;
        auto output=fs::absolute(cfg.outputPath).lexically_normal();
        auto manifest=fs::absolute(cfg.evidenceOutput).lexically_normal();
        fs::create_directories(output.parent_path()); fs::create_directories(manifest.parent_path());
        output=fs::canonical(output.parent_path())/output.filename();
        manifest=fs::canonical(manifest.parent_path())/manifest.filename();
        const auto name=output.filename().string();
        if(name.empty() || name=="." || name==".." || std::any_of(name.begin(),name.end(),[](unsigned char c){
            return !(c>='a'&&c<='z')&&!(c>='A'&&c<='Z')&&!(c>='0'&&c<='9')&&c!='_'&&c!='-'&&c!='.';
        }) || output==manifest || manifest.string()==output.string()+".topo-abi.json") {
            error="isolated-build requires distinct paths and a portable dylib basename";return result;
        }
        if (fs::exists(fs::symlink_status(output)) || fs::exists(fs::symlink_status(manifest)) ||
            fs::exists(fs::symlink_status(output.string()+".topo-abi.json"))) {
            error="isolated-build requires fresh output, ABI sidecar and evidence paths";return result;
        }
        fs::path stage;
        for (unsigned i=0;i<16;++i) {
            stage=manifest.parent_path()/(".topo-isolated-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            if (fs::create_directory(stage,ec)) break;
            stage.clear();
        }
        if (stage.empty()) {error="cannot create isolated evidence data directory";return result;}
        stage=fs::canonical(stage); fs::permissions(stage,fs::perms::owner_all);
        fs::create_directory(stage/"inputs");fs::create_directory(stage/"outputs");
        const auto originalDigest=dist::sha256File(source.string());
        fs::copy_file(source,stage/"inputs/source.cpp");
        if (!ev::validDigest(originalDigest) || dist::sha256File((stage/"inputs/source.cpp").string())!=originalDigest ||
            dist::sha256File(source.string())!=originalDigest) {error="source changed while preparing isolated snapshot";return result;}
        fs::permissions(stage/"inputs/source.cpp",fs::perms::owner_read);
        fs::permissions(stage/"inputs",fs::perms::owner_read|fs::perms::owner_exec);
        std::map<std::string,std::string> toolchainImages;
        if(!toolLibraries(compiler,linker,toolchainImages,error))return result;
        std::string profile="(version 1)\n(deny default)\n(allow process*)\n(allow sysctl-read)\n(allow mach-lookup)\n(allow file-read-metadata)\n";
        for(const fs::path& path : {fs::path("/usr/lib"),fs::path("/System/Library"),fs::path("/Library/Apple/System/Library"),
            fs::path("/private/var/db/dyld"),fs::path("/private/etc"),fs::path("/Library/Preferences"),fs::path("/dev"),compiler.parent_path().parent_path(),linker.parent_path().parent_path(),sdk,stage}) {
            auto quoted=quoteProfile(path);if(quoted.empty()){error="unsupported control byte in sandbox path";return result;}
            profile += "(allow file-read* (subpath "+quoted+"))\n";
        }
        for(const auto& [path,digest]:toolchainImages) {
            (void)digest;auto quoted=quoteProfile(path);if(quoted.empty()){error="unsupported toolchain path";return result;}
            profile += "(allow file-read* (literal "+quoted+"))\n";
        }
        profile += "(allow file-read* (literal \"/usr/bin/env\") (literal \"/dev/null\") (literal \"/dev/random\") (literal \"/dev/urandom\"))\n";
        profile += "(allow file-read* (literal \"/\") (literal "+quoteProfile(fs::current_path())+"))\n";
        profile += "(allow file-write* (subpath "+quoteProfile(stage/"outputs")+"))\n(deny network*)\n";
        if(!writeText(stage/"sandbox.sb",profile)){error="cannot write sandbox profile";return result;}
        const auto base=isolatedCppCompilePrefix(cfg,error);if(!error.empty())return result;
        std::vector<nlohmann::json> commands;
        auto run=[&](const std::vector<std::string>& args) {
            std::vector<std::string> sandbox={"-f",(stage/"sandbox.sb").string(),"/usr/bin/env","-i","PATH=/usr/bin:/bin",
                "TMPDIR="+(stage/"outputs").string(),"LC_ALL=C","SOURCE_DATE_EPOCH=0",compiler.string()};
            sandbox.insert(sandbox.end(),args.begin(),args.end());
            auto process=platform::runProcessCaptureWithTimeout("/usr/bin/sandbox-exec",sandbox,30000,cfg.verbose);
            commands.push_back({{"executable","/usr/bin/sandbox-exec"},{"argv",sandbox},{"exitCode",process.exitCode}});
            return process;
        };
        auto astArgs=base;astArgs.insert(astArgs.end(),{"-Xclang","-ast-dump=json","-fsyntax-only",(stage/"inputs/source.cpp").string()});
        auto astRun=run(astArgs);
        writeText(stage/"ast.stderr",astRun.stderrOutput);
        if(astRun.exitCode!=0){error="sandboxed compiler AST failed (exit "+std::to_string(astRun.exitCode)+"): "+astRun.stderrOutput;return result;}
        if(astRun.stdoutOutput.size()>16u*1024u*1024u){error="compiler AST exceeds evidence limit";return result;}
        nlohmann::json exports;
        auto ast=nlohmann::json::parse(astRun.stdoutOutput,nullptr,false);
        if(ast.is_discarded() || !exportedSignatures(ast,exports,error)) return result;
        if(!writeText(stage/"ast.json",astRun.stdoutOutput)){error="cannot preserve compiler AST";return result;}
        auto compileArgs=base;
        compileArgs.insert(compileArgs.end(),{"-nostdlib","-shared",
            "--ld-path="+linker.string(),(stage/"inputs/source.cpp").string(),"-o",(stage/"outputs/library.dylib").string(),
            "-MD","-MF",(stage/"outputs/dependencies.d").string(),"-Wl,-no_uuid","-Wl,-undefined,error",
            "-Wl,-install_name,@rpath/"+output.filename().string(),"-L"+(sdk/"usr/lib").string(),"-lSystem"});
        auto compileRun=run(compileArgs); writeText(stage/"compile.stderr",compileRun.stderrOutput);
        if(compileRun.exitCode!=0){error="sandboxed compiler/linker failed: "+compileRun.stderrOutput;return result;}
        if(dist::sha256File(compiler.string())!=cfg.evidenceCompilerSha256 || dist::sha256File(linker.string())!=cfg.evidenceLinkerSha256 ||
            dist::sha256File((stage/"inputs/source.cpp").string())!=originalDigest){error="isolated tool or copied input changed during build";return result;}
        for(const auto& [path,digest]:toolchainImages)if(dist::sha256File(path)!=digest){error="toolchain library changed during build";return result;}
        const auto binary=readText(stage/"outputs/library.dylib");
        if(binary.size()>16u*1024u*1024u || !inspectDylib(binary,exports,error)) return result;
        // Only the copied source and the selected SDK settings may occur in the
        // dependency file. Any additional header requires a wider input model.
        auto deps=readText(stage/"outputs/dependencies.d");
        auto escapeMake=[](const std::string& path){std::string s;for(char c:path){if(c==' '||c=='#'||c=='\\')s+='\\';s+=c;}return s;};
        const auto colon=deps.find(':'); if(colon==std::string::npos){error="missing compiler dependency evidence";return result;}
        deps=deps.substr(colon+1);
        for(const auto& path:{stage/"inputs/source.cpp",sdk/"SDKSettings.json"}) {
            const auto needle=escapeMake(path.string());size_t at=0;
            while((at=deps.find(needle,at))!=std::string::npos)deps.erase(at,needle.size());
        }
        for(char c:deps)if(c!=' '&&c!='\t'&&c!='\r'&&c!='\n'&&c!='\\'){error="compiler read an input outside the restricted snapshot/SDK settings";return result;}
        fs::copy_file(stage/"outputs/library.dylib",output);
        nlohmann::json abi={{"schemaVersion",1},{"type","topo.native-abi"},{"artifact",{{"sha256",dist::sha256File(output.string())}}},
            {"targetTriple","arm64-apple-darwin"},{"binaryKind","dylib"},{"exports",exports}};
        const fs::path abiPath=output.string()+".topo-abi.json";
        if(!writeText(abiPath,abi.dump(2)+"\n")){error="cannot write native ABI sidecar";return result;}
        ev::Record record;record.action.kind="cpp-isolated-build";record.action.version="1";
        record.action.toolIdentity="sha256:"+cfg.evidenceCompilerSha256;record.action.argv=compileArgs;record.action.cwd=fs::current_path().string();
        record.action.environment={{"PATH","/usr/bin:/bin"},{"TMPDIR",(stage/"outputs").string()},{"LC_ALL","C"},{"SOURCE_DATE_EPOCH","0"}};
        record.action.parameters={{"capture","isolated-direct-cpp"},{"commands",commands},{"targetTriple",Target},
            {"linker",{{"path",linker.string()},{"sha256",cfg.evidenceLinkerSha256}}},
            {"toolchainImages",toolchainImages},
            {"sandboxSha256",dist::sha256File("/usr/bin/sandbox-exec")},{"environmentToolSha256",dist::sha256File("/usr/bin/env")},
            {"sdkPath",sdk.string()},{"sdkSettingsSha256",dist::sha256File((sdk/"SDKSettings.json").string())},
            {"systemStubSha256",dist::sha256File((sdk/"usr/lib/libSystem.tbd").string())},
            {"nativeDependencies",{"/usr/lib/libSystem.B.dylib"}},
            {"native",{{"abi","c"},{"targetTriple","arm64-apple-darwin"},{"binaryKind","dylib"},{"exports",exports}}},
            {"trustAssumptions",{"macOS sandbox and system runtime","compiler/linker transitive toolchain libraries","exclusive owner control of evidence directory"}}};
        auto add=[&](const fs::path& root,const std::string& rootId,const fs::path& path,std::vector<ev::FileEntry>& entries){
            ev::FileEntry entry;if(!ev::snapshotFile(root,rootId,path.generic_string(),entry,error))return false;entries.push_back(entry);return true;};
        if(!add(stage,"build","inputs/source.cpp",record.action.inputs) || !add(output.parent_path(),"artifact",output.filename(),record.outputs) ||
            !add(output.parent_path(),"artifact",abiPath.filename(),record.metadata))return result;
        for(const auto& path:{"sandbox.sb","ast.json","ast.stderr","compile.stderr","outputs/dependencies.d","outputs/library.dylib"})
            if(!add(stage,"build",path,record.metadata))return result;
        record.producer={{"builder","topo-build-llvm-cpp"},{"observation","isolated-process-dispatch"},{"isolation","macos-sandbox-exec"},
            {"coverage","restricted-c-abi-with-platform-trust"},{"buildRoot",stage.string()},{"artifactRoot",output.parent_path().string()}};
        record.complete=false;ev::sealRecord(record);
        ev::Manifest evidence{ev::SchemaVersion,{record},{record.id}};
        if(!ev::writeManifest(manifest,evidence,error))return result;
        result.exitCode=0;result.outputFiles={output.string(),abiPath.string(),manifest.string()};return result;
    } catch(const std::exception& e){error=std::string("isolated-build failed: ")+e.what();return result;}
#endif
}
} // namespace topo::build
