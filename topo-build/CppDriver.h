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

namespace evidence { class ExecutionRecorder; }

/// Emit the exact ordered direct-compile commands used for audited builds.
/// The standard compilation-database fields are augmented with source/tool
/// digests. A BuildConfig.checkModelPath requires its recorded SHA-256 and
/// must still match this model at execution time; otherwise compilation fails.
bool writeCppCheckModel(const BuildConfig& cfg, const std::filesystem::path& tempDir,
                        const std::filesystem::path& output, std::string& error);
bool validateCppCheckModel(const BuildConfig& cfg, const std::filesystem::path& tempDir,
                           std::string& error);

/// JIT export map: entries tracking original and current mangled names.
using JitExportMap = std::vector<topo::backend::JitExportEntry>;

/// Compile C++ sources to LLVM IR (.ll files).
DriverResult compileCpp(const BuildConfig& cfg, const std::filesystem::path& tempDir,
                        evidence::ExecutionRecorder* recorder = nullptr);

/// Ninja-based incremental compile: generates build.ninja, runs ninja,
/// returns .ll file paths from the cache IR directory.
/// A checked model, evidence mode, or recorder forces direct compilation:
/// ordinary Ninja outputs have no independently verified build provenance.
DriverResult compileCppIncremental(const BuildConfig& cfg, const std::filesystem::path& cacheDir,
                                   evidence::ExecutionRecorder* recorder = nullptr);

/// Link optimized IR -> final binary (exe/shared/static) using clang++.
DriverResult linkCpp(const BuildConfig& cfg,
                     const std::string& optIRPath,
                     const std::filesystem::path& tempDir,
                     const JitExportMap& jitExports,
                     evidence::ExecutionRecorder* recorder = nullptr);

} // namespace topo::build

#endif // TOPO_BUILD_CPPDRIVER_H
