#ifndef TOPO_BUILD_CPPDRIVER_H
#define TOPO_BUILD_CPPDRIVER_H

#include "topo/Build/BuildConfig.h"
#include "topo/Backend/BackendTypes.h"

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace topo::build {

/// JIT export map: entries tracking original and current mangled names.
using JitExportMap = std::vector<topo::backend::JitExportEntry>;

/// Compile C++ sources to LLVM IR (.ll files).
DriverResult compileCpp(const BuildConfig& cfg, const std::filesystem::path& tempDir);

/// Ninja-based incremental compile: generates build.ninja, runs ninja,
/// returns .ll file paths from the cache IR directory.
DriverResult compileCppIncremental(const BuildConfig& cfg, const std::filesystem::path& cacheDir);

/// Link optimized IR -> final binary (exe/shared/static) using clang++.
DriverResult linkCpp(const BuildConfig& cfg,
                     const std::string& optIRPath,
                     const std::filesystem::path& tempDir,
                     const JitExportMap& jitExports);

} // namespace topo::build

#endif // TOPO_BUILD_CPPDRIVER_H
