#ifndef TOPO_BUILD_ISOLATEDCPPBUILD_H
#define TOPO_BUILD_ISOLATEDCPPBUILD_H

#include "topo/Build/BuildConfig.h"
#include <string>

namespace topo::build {

// Shared frontend arguments for model generation, AST inspection, and the
// actual isolated compiler. Link-only flags are appended by the builder.
std::vector<std::string> isolatedCppCompilePrefix(const BuildConfig& config, std::string& error);

// Restricted macOS ARM64 C ABI build: one self-contained source, explicit
// compiler/linker digest pins, no project flags/includes/libraries or Topo
// transforms. Runs real compiler commands under the OS sandbox, preserving
// a read-only copied input and detached evidence. Toolchain/runtime libraries
// remain explicit platform trust assumptions; the manifest stays incomplete.
DriverResult buildIsolatedCpp(const BuildConfig& config, std::string& error);

} // namespace topo::build
#endif
