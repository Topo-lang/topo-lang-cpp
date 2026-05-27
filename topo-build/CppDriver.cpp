#include "CppDriver.h"

#include "NinjaGen.h"
#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"
#include "topo/Platform/ToolResolution.h"

#include <future>
#include <iostream>

namespace fs = std::filesystem;

namespace topo::build {

namespace {
// macOS: get sysroot args for the bundled clang++ to find system headers/libs
std::vector<std::string> getMacOSSysrootArgs() {
    std::vector<std::string> args;
    if constexpr (platform::IsMacOS) {
        auto result = platform::runProcessCapture("xcrun", {"--show-sdk-path"});
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

DriverResult compileCpp(const BuildConfig& cfg, const fs::path& tempDir) {
    namespace plat = platform;
    DriverResult result;

    std::cerr << "[3/7] Compiling " << cfg.sources.size() << " source file(s) to LLVM IR...\n";

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

    std::vector<std::future<int>> compileFutures;

    for (const auto& src : cfg.sources) {
        fs::path srcPath(src);
        std::string stem = srcPath.stem().string();
        std::string llFile = (tempDir / (stem + ".ll")).string();
        result.outputFiles.push_back(llFile);

        std::vector<std::string> args = baseCompileArgs;
        args.push_back(src);
        args.push_back("-o");
        args.push_back(llFile);

        bool verbose = cfg.verbose;
        std::string exe = cfg.hostCompilerPath;
        compileFutures.push_back(std::async(
            std::launch::async, [exe, args, verbose]() { return plat::runProcess(exe, args, verbose).exitCode; }));
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

DriverResult compileCppIncremental(const BuildConfig& cfg, const fs::path& cacheDir) {
    namespace plat = platform;
    DriverResult result;

    fs::path irDir = cacheDir / "ir";
    fs::path ninjaPath = cacheDir / "build.ninja";

    std::cerr << "[3/7] Compiling " << cfg.sources.size() << " source file(s) to LLVM IR (incremental)...\n";

    // Generate/update build.ninja
    if (!NinjaGen::generate(cfg, ninjaPath, irDir)) {
        result.exitCode = 1;
        return result;
    }

    // Run ninja
    int ninjaRet = NinjaGen::run(cacheDir, cfg.verbose);
    if (ninjaRet != 0) {
        std::cerr << "error: ninja build failed (exit " << ninjaRet << ")\n";
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

    return result;
}

DriverResult linkCpp(const BuildConfig& cfg,
                     const std::string& optIRPath,
                     const fs::path& tempDir,
                     const JitExportMap& jitExports) {
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

        result.exitCode = plat::runProcess(cfg.hostCompilerPath, args, cfg.verbose).exitCode;
        break;
    }
    case OutputType::Shared: {
        std::cerr << "[7/7] Linking shared library...\n";
        std::vector<std::string> args = {"-shared", "-std=" + cfg.standard};
        args.insert(args.end(), sysrootArgs.begin(), sysrootArgs.end());
        args.insert(args.end(), optArgs.begin(), optArgs.end());
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
        result.exitCode = plat::runProcess(cfg.hostCompilerPath, args, cfg.verbose).exitCode;
        break;
    }
    case OutputType::Static: {
        std::cerr << "[7/7] Archiving static library...\n";
        std::string objPath = (tempDir / ("output" + std::string(plat::ObjectFileSuffix))).string();
        std::vector<std::string> compileArgs = {"-c", "-std=" + cfg.standard};
        compileArgs.insert(compileArgs.end(), sysrootArgs.begin(), sysrootArgs.end());
        compileArgs.insert(compileArgs.end(), optArgs.begin(), optArgs.end());
        compileArgs.push_back(optIRPath);
        compileArgs.push_back("-o");
        compileArgs.push_back(objPath);
        result.exitCode = plat::runProcess(cfg.hostCompilerPath, compileArgs, cfg.verbose).exitCode;
        if (result.exitCode != 0) break;

        std::vector<std::string> arArgs = {"rcs", cfg.outputPath, objPath};
        result.exitCode = plat::runProcess(plat::resolveLLVMTool("llvm-ar"), arArgs, cfg.verbose).exitCode;
        break;
    }
    }

    return result;
}

} // namespace topo::build
