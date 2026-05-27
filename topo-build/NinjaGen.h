#ifndef TOPO_BUILD_NINJAGEN_H
#define TOPO_BUILD_NINJAGEN_H

#include <filesystem>
#include <string>
#include <vector>

namespace topo::build {

struct BuildConfig;

class NinjaGen {
public:
    /// Generate a build.ninja file from BuildConfig.
    /// @param cfg          Build configuration (sources, compiler, flags, etc.)
    /// @param ninjaPath    Output path for build.ninja (e.g., .topo-cache/build.ninja)
    /// @param irOutputDir  Directory for .ll output files (e.g., .topo-cache/ir/)
    /// @return true on success
    static bool generate(const BuildConfig& cfg,
                         const std::filesystem::path& ninjaPath,
                         const std::filesystem::path& irOutputDir);

    /// Run ninja in the given directory.
    /// @param ninjaDir  Directory containing build.ninja
    /// @param verbose   If true, pass -v to ninja
    /// @return ninja exit code (0 = success)
    static int run(const std::filesystem::path& ninjaDir, bool verbose = false);

    /// Check if ninja is available on the system.
    static bool isAvailable();

    /// Resolve the ninja executable path.
    /// Checks TOPO_NINJA_PATH env var, then PATH.
    /// Returns empty string if not found.
    static std::string resolveNinja();

    /// Flatten a source file path into a unique output name.
    /// e.g., "src/main.cpp" -> "src_main", "lib/core/engine.cpp" -> "lib_core_engine"
    static std::string flattenSourceName(const std::string& srcPath, const std::filesystem::path& baseDir);
};

} // namespace topo::build

#endif // TOPO_BUILD_NINJAGEN_H
