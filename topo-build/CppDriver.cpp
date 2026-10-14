#include "CppDriver.h"
#include "IsolatedCppBuild.h"
#include "topo/Distribution/Sha256.h"
#include "topo/Build/ExecutionEvidence.h"

#include "NinjaGen.h"
#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"
#include "topo/Platform/ToolResolution.h"

#include <future>
#include <fstream>
#include <cstdlib>
#include <iostream>

namespace fs = std::filesystem;

namespace topo::build {

namespace {
// macOS: get sysroot args for the bundled clang++ to find system headers/libs
std::vector<std::string> getMacOSSysrootArgs() {
    std::vector<std::string> args;
    if constexpr (platform::IsMacOS) {
        auto result = platform::runProcessCapture("/usr/bin/xcrun", {"--show-sdk-path"});
        if (result.exitCode == 0 && !result.stdoutOutput.empty()) {
            std::string sdkPath = result.stdoutOutput;
            while (!sdkPath.empty() && (sdkPath.back() == '\n' || sdkPath.back() == '\r'))
                sdkPath.pop_back();
            args.push_back("-isysroot");
            args.push_back(sdkPath);
        }
    }
    return args;
}
} // anonymous namespace

static std::vector<std::string> cppCompilePrefix(const BuildConfig& cfg) {
    namespace plat = platform;
    // Build shared compile arguments. Dev mode includes `-g` so the produced
    // binary is debuggable by lldb / `topo debug` out of the box; aggressive
    // mode strips for release artifacts. Without this, `topo debug` against
    // a stock `topo build` binary fails with "no breakpoint locations
    // resolved" because the IR has no DWARF.
    std::vector<std::string> baseCompileArgs = {
        "-std=" + cfg.standard, "-S", "-emit-llvm", "-O2", "-Xclang", "-disable-llvm-passes"};
    if (cfg.buildMode != BuildMode::Aggressive) {
        baseCompileArgs.push_back("-g");
    }
    // macOS: bundled clang++ needs explicit SDK path to find system headers
    {
        auto sysrootArgs = getMacOSSysrootArgs();
        baseCompileArgs.insert(baseCompileArgs.end(), sysrootArgs.begin(), sysrootArgs.end());
    }
    // Note: -flto=thin is only applied at link step (not compile step).
    // Compile step generates text IR that topo-build parses directly;
    // ThinLTO summary entries in text IR cause IR reader errors.
    if constexpr (!plat::IsWindows) {
        if (cfg.outputType == OutputType::Shared) {
            baseCompileArgs.push_back("-fPIC");
            baseCompileArgs.push_back("-fvisibility=hidden");
        }
    }
    for (const auto& dir : cfg.includeDirs) {
        baseCompileArgs.push_back("-I");
        baseCompileArgs.push_back(dir);
    }
    if (cfg.embedIR) {
        baseCompileArgs.push_back("-DTOPO_HAS_JIT");
    }
    if (cfg.adaptiveCfg.isEnabled()) {
        baseCompileArgs.push_back("-DTOPO_HAS_ADAPTIVE");
    }
    // [build.cpp].flags — free-form user flags appended LAST so they can
    // override the built-ins above (clang last-flag-wins).
    baseCompileArgs.insert(baseCompileArgs.end(), cfg.cppFlags.begin(), cfg.cppFlags.end());

    return baseCompileArgs;
}

static nlohmann::json cppCommandModel(const BuildConfig& cfg, const fs::path& tempDir, std::string& error) {
    try {
        std::string compiler=platform::findSpawnableOnPath(cfg.hostCompilerPath);
        if(compiler.empty()){error="cannot resolve check-model compiler";return nullptr;}
        const auto compilerDigest=dist::sha256File(compiler);
        if(!evidence::validDigest(compilerDigest)){error="cannot hash check-model compiler";return nullptr;}
        const auto prefix=cfg.evidenceMode=="isolated-build" ? isolatedCppCompilePrefix(cfg,error) : cppCompilePrefix(cfg);
        if(!error.empty())return nullptr;
        nlohmann::json environment=nlohmann::json::object();
        for(const char* name:{"PATH","CPATH","CPLUS_INCLUDE_PATH","C_INCLUDE_PATH","SDKROOT","MACOSX_DEPLOYMENT_TARGET",
                "CCC_OVERRIDE_OPTIONS","CLANG_CONFIG_FILE_SYSTEM_DIR","CLANG_CONFIG_FILE_USER_DIR"})
            if(const char* value=std::getenv(name))environment[name]=value;
        auto model=nlohmann::json::array();
        for(const auto& source:cfg.sources){
            const auto path=fs::absolute(source).lexically_normal();
            const auto sourceDigest=dist::sha256File(path.string());
            if(!evidence::validDigest(sourceDigest)){error="cannot hash check-model source: "+source;return nullptr;}
            std::vector<std::string> args{compiler};args.insert(args.end(),prefix.begin(),prefix.end());
            args.push_back(path.string());
            if(cfg.evidenceMode=="isolated-build")args.push_back("-fsyntax-only");
            else{
                const auto stem=path.stem().string()+"-"+dist::sha256Hex(path.generic_string()).substr(0,16);
                args.push_back("-o");args.push_back((fs::absolute(tempDir)/(stem+".ll")).string());
            }
            model.push_back({{"directory",fs::current_path().string()},{"file",path.string()},{"arguments",args},
                {"topoModelVersion",1},{"topoModelKind",cfg.evidenceMode=="isolated-build"?"isolated-frontend":"direct-compile"},
                {"topoCompilerSha256",compilerDigest},{"topoSourceSha256",sourceDigest},
                {"topoSourceCanonical",fs::canonical(path).string()},{"topoEnvironment",environment}});
        }
        return model;
    }catch(const std::exception& exception){error=std::string("cannot generate C++ check model: ")+exception.what();return nullptr;}
}

static nlohmann::json checkedCppCommandModel(const BuildConfig& cfg,const fs::path& tempDir,std::string& error){
    auto expected=cppCommandModel(cfg,tempDir,error);if(expected.is_null())return expected;
    if(cfg.checkModelPath.empty())return expected;
    if(!evidence::validDigest(cfg.checkModelSha256)||dist::sha256File(cfg.checkModelPath)!=cfg.checkModelSha256){
        error="checked C++ command model is missing or changed";return nullptr;
    }
    std::ifstream input(cfg.checkModelPath,std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),{}};
    // Exact canonical bytes reject duplicate keys, alternate commands and stale
    // source/tool/environment/config inputs before any compiler is launched.
    if(bytes!=expected.dump(2)+"\n"){
        error="C++ build command model differs from the model used by topo-check";return nullptr;
    }
    return expected;
}

bool writeCppCheckModel(const BuildConfig& cfg,const fs::path& tempDir,const fs::path& output,std::string& error){
    auto model=cppCommandModel(cfg,tempDir,error);if(model.is_null())return false;
    std::ofstream file(output,std::ios::binary);file<<model.dump(2)<<"\n";file.close();
    if(!file){error="cannot write C++ check model";return false;}return true;
}
bool validateCppCheckModel(const BuildConfig& cfg,const fs::path& tempDir,std::string& error){
    return !checkedCppCommandModel(cfg,tempDir,error).is_null();
}

DriverResult compileCpp(const BuildConfig& cfg, const fs::path& tempDir, evidence::ExecutionRecorder* recorder) {
    namespace plat = platform;
    DriverResult result;

    if (!cfg.evidenceMode.empty() && cfg.evidenceMode != "commands") {
        std::cerr << "error: direct C++ compilation supports commands evidence only; "
                     "isolated-build requires the isolated builder\n";
        result.exitCode = 1;
        return result;
    }

    std::cerr << "[3/7] Compiling " << cfg.sources.size() << " source file(s) to LLVM IR...\n";

    std::string modelError;
    auto model=checkedCppCommandModel(cfg,tempDir,modelError);
    if(model.is_null()){
        std::cerr<<"error: "<<modelError<<"\n";result.exitCode=1;return result;
    }
    std::vector<std::future<int>> compileFutures;
    for(const auto& command:model){
        auto args=command.at("arguments").get<std::vector<std::string>>();
        const auto exe=args.front();args.erase(args.begin());
        const auto llFile=args.back();result.outputFiles.push_back(llFile);
        std::vector<std::string> inputs{command.at("file").get<std::string>()};
        if(!cfg.checkModelPath.empty())inputs.push_back(cfg.checkModelPath);
        const bool verbose=cfg.verbose;
        compileFutures.push_back(std::async(std::launch::async,[exe,args,verbose,recorder,inputs,llFile](){
            return recorder?recorder->run("cpp-compile",exe,args,inputs,{llFile},verbose).exitCode
                           :plat::runProcess(exe,args,verbose).exitCode;
        }));
    }

    // Wait for all compilations
    for (size_t i = 0; i < compileFutures.size(); ++i) {
        int ret = compileFutures[i].get();
        if (ret != 0) {
            std::cerr << "error: compilation failed for '" << cfg.sources[i] << "' (exit " << ret << ")\n";
            result.exitCode = 1;
            return result;
        }
    }

    return result;
}

DriverResult compileCppIncremental(const BuildConfig& cfg, const fs::path& cacheDir, evidence::ExecutionRecorder* recorder) {
    namespace plat = platform;
    DriverResult result;

    fs::path irDir = cacheDir / "ir";
    // The ordinary Ninja cache has mtime/command identity, without a complete
    // input closure or authenticated producer. Every audit/evidence caller,
    // including direct API consumers, must execute the checked command model.
    if (recorder || !cfg.evidenceMode.empty() || !cfg.checkModelPath.empty())
        return compileCpp(cfg, irDir, recorder);

    fs::path ninjaPath = cacheDir / "build.ninja";

    std::cerr << "[3/7] Compiling " << cfg.sources.size() << " source file(s) to LLVM IR (incremental)...\n";

    // Generate/update build.ninja
    if (!NinjaGen::generate(cfg, ninjaPath, irDir)) {
        result.exitCode = 1;
        return result;
    }

    // Collect output .ll files
    fs::path baseDir = cacheDir.parent_path(); // .topo-cache/ -> project dir
    for (const auto& src : cfg.sources) {
        std::string stem = NinjaGen::flattenSourceName(src, baseDir);
        std::string llFile = (irDir / (stem + ".ll")).string();
        result.outputFiles.push_back(llFile);
    }

    int ninjaRet = NinjaGen::run(cacheDir, cfg.verbose);
    if (ninjaRet != 0) {
        std::cerr << "error: ninja build failed (exit " << ninjaRet << ")\n";
        result.exitCode = 1;
        return result;
    }

    return result;
}

DriverResult linkCpp(const BuildConfig& cfg,
                     const std::string& optIRPath,
                     const fs::path& tempDir,
                     const JitExportMap& jitExports,
                     evidence::ExecutionRecorder* recorder) {
    namespace plat = platform;
    DriverResult result;

    // Shared optimization flags for linking
    std::vector<std::string> optArgs = {"-O" + std::to_string(static_cast<int>(cfg.optLevel))};
    if (cfg.buildMode == BuildMode::Aggressive) {
        optArgs.push_back("-flto=thin");
        if constexpr (!plat::IsMacOS) {
            optArgs.push_back("-fuse-ld=lld");
        }
    } else {
        // Dev mode: `-g` at link time tells clang to either embed DWARF
        // (Linux/Windows) or run dsymutil (macOS) so the binary is
        // debuggable by lldb / `topo debug`. The compile step already
        // emitted DWARF into the IR; without `-g` at link the linker on
        // macOS leaves dangling debug-map entries pointing at intermediate
        // objects (which the temp dir cleanup then deletes), producing a
        // stripped final binary.
        optArgs.push_back("-g");
    }

    // macOS: bundled clang++ needs explicit SDK path for linking
    auto sysrootArgs = getMacOSSysrootArgs();

    switch (cfg.outputType) {
    case OutputType::Exe: {
        std::cerr << "[7/7] Linking executable...\n";
        std::vector<std::string> args = {"-std=" + cfg.standard};
        args.insert(args.end(), sysrootArgs.begin(), sysrootArgs.end());
        args.insert(args.end(), optArgs.begin(), optArgs.end());
        // [build.cpp].flags apply at link too (e.g. -fsanitize needs both
        // an instrumented compile AND the runtime linked in).
        args.insert(args.end(), cfg.cppFlags.begin(), cfg.cppFlags.end());
        args.push_back(optIRPath);
        args.push_back("-o");
        args.push_back(cfg.outputPath);
        for (const auto& dir : cfg.linkDirs)
            args.push_back("-L" + dir);
        for (const auto& lib : cfg.linkLibs)
            args.push_back("-l" + lib);

        if constexpr (plat::IsWindows) {
            for (const auto& entry : jitExports) {
                if (entry.currentMangledName.empty()) continue;
                if (entry.currentMangledName == entry.originalMangledName) {
                    args.push_back("-Wl,/EXPORT:" + entry.originalMangledName);
                } else {
                    args.push_back("-Wl,/EXPORT:" + entry.originalMangledName + "=" + entry.currentMangledName);
                }
            }
        }

        result.exitCode = recorder ? recorder->run("cpp-link", cfg.hostCompilerPath, args,
                                                   {optIRPath}, {cfg.outputPath}, cfg.verbose).exitCode
                                   : plat::runProcess(cfg.hostCompilerPath, args, cfg.verbose).exitCode;
        break;
    }
    case OutputType::Shared: {
        std::cerr << "[7/7] Linking shared library...\n";
        std::vector<std::string> args = {"-shared", "-std=" + cfg.standard};
        args.insert(args.end(), sysrootArgs.begin(), sysrootArgs.end());
        args.insert(args.end(), optArgs.begin(), optArgs.end());
        args.insert(args.end(), cfg.cppFlags.begin(), cfg.cppFlags.end());
        if constexpr (!plat::IsWindows) {
            args.push_back("-fPIC");
        }
        args.push_back(optIRPath);
        args.push_back("-o");
        args.push_back(cfg.outputPath);
        for (const auto& dir : cfg.linkDirs)
            args.push_back("-L" + dir);
        for (const auto& lib : cfg.linkLibs)
            args.push_back("-l" + lib);
        result.exitCode = recorder ? recorder->run("cpp-link", cfg.hostCompilerPath, args,
                                                   {optIRPath}, {cfg.outputPath}, cfg.verbose).exitCode
                                   : plat::runProcess(cfg.hostCompilerPath, args, cfg.verbose).exitCode;
        break;
    }
    case OutputType::Static: {
        std::cerr << "[7/7] Archiving static library...\n";
        std::string objPath = (tempDir / ("output" + std::string(plat::ObjectFileSuffix))).string();
        std::vector<std::string> compileArgs = {"-c", "-std=" + cfg.standard};
        compileArgs.insert(compileArgs.end(), sysrootArgs.begin(), sysrootArgs.end());
        compileArgs.insert(compileArgs.end(), optArgs.begin(), optArgs.end());
        compileArgs.insert(compileArgs.end(), cfg.cppFlags.begin(), cfg.cppFlags.end());
        compileArgs.push_back(optIRPath);
        compileArgs.push_back("-o");
        compileArgs.push_back(objPath);
        result.exitCode = recorder ? recorder->run("cpp-object", cfg.hostCompilerPath, compileArgs,
                                                   {optIRPath}, {objPath}, cfg.verbose).exitCode
                                   : plat::runProcess(cfg.hostCompilerPath, compileArgs, cfg.verbose).exitCode;
        if (result.exitCode != 0) break;

        std::vector<std::string> arArgs = {"rcs", cfg.outputPath, objPath};
        result.exitCode = recorder ? recorder->run("cpp-archive", plat::llvmToolPath("llvm-ar"), arArgs,
                                                   {objPath}, {cfg.outputPath}, cfg.verbose).exitCode
                                   : plat::runProcess(plat::llvmToolPath("llvm-ar"), arArgs, cfg.verbose).exitCode;
        break;
    }
    }

    return result;
}

} // namespace topo::build
