#include "NinjaGen.h"
#include "topo/Build/BuildConfig.h"
#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace topo::build {

/// Escape a path for ninja ($ -> $$, space -> $ , colon -> $:).
static std::string ninjaEscape(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        if (c == '$')
            result += "$$";
        else if (c == ' ')
            result += "$ ";
        else if (c == ':')
            result += "$:";
        else
            result += c;
    }
    return result;
}

std::string NinjaGen::flattenSourceName(const std::string& srcPath, const fs::path& baseDir) {
    // Make relative to baseDir if possible
    fs::path src(srcPath);
    std::string relative;
    if (src.is_relative()) {
        relative = src.generic_string();
    } else {
        std::error_code ec;
        auto rel = fs::relative(src, baseDir, ec);
        if (!ec && !rel.empty())
            relative = rel.generic_string();
        else
            relative = src.generic_string();
    }

    // Remove extension
    auto dotPos = relative.rfind('.');
    if (dotPos != std::string::npos) relative = relative.substr(0, dotPos);

    // Replace / and \ with _
    std::string result;
    result.reserve(relative.size());
    for (char c : relative) {
        if (c == '/' || c == '\\')
            result += '_';
        else
            result += c;
    }

    return result;
}

bool NinjaGen::generate(const BuildConfig& cfg, const fs::path& ninjaPath, const fs::path& irOutputDir) {
    namespace plat = platform;

    std::ofstream ofs(ninjaPath);
    if (!ofs.is_open()) {
        std::cerr << "error: cannot write " << ninjaPath.generic_string() << "\n";
        return false;
    }

    fs::path baseDir = ninjaPath.parent_path().parent_path(); // .topo-cache/ -> project dir

    // Build the compile command. cfg.hostCompilerPath is resolved on THIS host
    // (by the BYO toolchain resolver), so the emitted build.ninja is host-local
    // — it must be regenerated per machine and never committed or shipped (the
    // enclosing .topo-cache/ is a build artifact).
    std::ostringstream cmd;
    cmd << ninjaEscape(fs::path(cfg.hostCompilerPath).generic_string());
    cmd << " -std=" << cfg.standard;
    cmd << " -S -emit-llvm -O2 -Xclang -disable-llvm-passes";
    // Dev mode emits DWARF so the linked binary is debuggable by lldb /
    // `topo debug` out of the box; aggressive (release) mode strips. Mirrors
    // the matching branch in CppDriver.cpp (non-incremental path).
    if (cfg.buildMode != BuildMode::Aggressive) {
        cmd << " -g";
    }

    // macOS: bundled clang++ needs explicit SDK path to find system headers
    if constexpr (plat::IsMacOS) {
        auto sdkResult = plat::runProcessCapture("xcrun", {"--show-sdk-path"});
        if (sdkResult.exitCode == 0 && !sdkResult.stdoutOutput.empty()) {
            std::string sdkPath = sdkResult.stdoutOutput;
            while (!sdkPath.empty() && (sdkPath.back() == '\n' || sdkPath.back() == '\r'))
                sdkPath.pop_back();
            cmd << " -isysroot " << ninjaEscape(sdkPath);
        }
    }

    if constexpr (!plat::IsWindows) {
        if (cfg.outputType == OutputType::Shared) {
            cmd << " -fPIC -fvisibility=hidden";
        }
    }

    for (const auto& dir : cfg.includeDirs) {
        cmd << " -I" << ninjaEscape(fs::path(dir).generic_string());
    }

    if (cfg.embedIR) cmd << " -DTOPO_HAS_JIT";
    if (cfg.adaptiveCfg.isEnabled()) cmd << " -DTOPO_HAS_ADAPTIVE";

    // -MD -MF for dependency tracking
    cmd << " -MD -MF $DEP_FILE $in -o $out";

    std::string compileCmd = cmd.str();

    // Write ninja file
    ofs << "ninja_required_version = 1.3\n\n";

    ofs << "rule cc\n";
    ofs << "  command = " << compileCmd << "\n";
    ofs << "  description = CC $in\n";
    ofs << "  deps = gcc\n";
    ofs << "  depfile = $DEP_FILE\n\n";

    // Write build edges for each source file
    for (const auto& src : cfg.sources) {
        std::string stem = flattenSourceName(src, baseDir);
        fs::path outFile = irOutputDir / (stem + ".ll");
        fs::path depFile = irOutputDir / (stem + ".ll.d");

        std::string srcGeneric = fs::path(src).generic_string();
        std::string outGeneric = outFile.generic_string();
        std::string depGeneric = depFile.generic_string();

        ofs << "build " << ninjaEscape(outGeneric) << ": cc " << ninjaEscape(srcGeneric) << "\n";
        ofs << "  DEP_FILE = " << ninjaEscape(depGeneric) << "\n\n";
    }

    return true;
}

int NinjaGen::run(const fs::path& ninjaDir, bool verbose) {
    std::string ninja = resolveNinja();
    if (ninja.empty()) return -1;

    std::vector<std::string> args = {"-C", ninjaDir.generic_string()};
    if (verbose) args.push_back("-v");

    return platform::runProcess(ninja, args, verbose).exitCode;
}

bool NinjaGen::isAvailable() {
    return !resolveNinja().empty();
}

std::string NinjaGen::resolveNinja() {
    // 1. TOPO_NINJA_PATH env var
    if (const char* envPath = std::getenv("TOPO_NINJA_PATH")) {
        std::string path(envPath);
        if (!path.empty() && fs::exists(path)) return path;
    }

    // 2. Check if ninja is in PATH
    auto result = platform::runProcessCapture("ninja", {"--version"});
    if (result.exitCode == 0) return "ninja";

    return "";
}

} // namespace topo::build
