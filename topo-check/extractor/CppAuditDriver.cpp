#include "CppAuditDriver.h"
#include "../analysis/CppCompileModel.h"
#include <clang/Driver/Types.h>
#include <clang/Options/Options.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Option/Arg.h>
#include <llvm/Option/ArgList.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <filesystem>

namespace topo::check {
namespace {
namespace options = clang::options;
namespace types = clang::driver::types;

enum class ToolChain { Other, AMDGPU, NVPTX };
ToolChain toolChain(const llvm::Triple& target) {
    switch (target.getOS()) {
    case llvm::Triple::CUDA: return ToolChain::NVPTX;
    case llvm::Triple::AMDHSA:
        return target.getArch() == llvm::Triple::spirv64 ? ToolChain::Other : ToolChain::AMDGPU;
    case llvm::Triple::AMDPAL:
    case llvm::Triple::Mesa3D: return ToolChain::AMDGPU;
    default: break;
    }
#if LLVM_VERSION_MAJOR >= 23
    // LLVM 23 also selects AMDGPU by architecture in the default OS branch.
    // The explicit OS branches take precedence, even for an AMDGPU triple.
    switch (target.getOS()) {
    case llvm::Triple::AIX: case llvm::Triple::Haiku:
    case llvm::Triple::Darwin: case llvm::Triple::MacOSX:
    case llvm::Triple::IOS: case llvm::Triple::TvOS:
    case llvm::Triple::WatchOS: case llvm::Triple::XROS:
    case llvm::Triple::DriverKit: case llvm::Triple::DragonFly:
    case llvm::Triple::OpenBSD: case llvm::Triple::NetBSD:
    case llvm::Triple::FreeBSD: case llvm::Triple::Linux:
    case llvm::Triple::ELFIAMCU: case llvm::Triple::Fuchsia:
    case llvm::Triple::Managarm: case llvm::Triple::Serenity:
    case llvm::Triple::Solaris: case llvm::Triple::UEFI:
    case llvm::Triple::Win32: case llvm::Triple::PS4:
    case llvm::Triple::PS5: case llvm::Triple::Hurd:
    case llvm::Triple::LiteOS: case llvm::Triple::ZOS:
    case llvm::Triple::Vulkan: case llvm::Triple::ShaderModel:
    case llvm::Triple::ChipStar: return ToolChain::Other;
    default:
        if (target.isAMDGPU()) return ToolChain::AMDGPU;
    }
#endif
    return ToolChain::Other;
}
}

std::string cppAuditDriverRejection(const CppCompileUnit& unit) {
    // The extractor supplies its separate source argument after semantic argv;
    // the internal frontend bridge appends it at the same position. Parse that
    // exact ordering, including values consumed by -D, -I, -x and -target.
    std::vector<const char*> argv;
    for (const auto& arg : unit.arguments) argv.push_back(arg.c_str());
    argv.push_back(unit.file.c_str());
    unsigned missingIndex = 0, missingCount = 0;
    const auto args = clang::getDriverOptTable().ParseArgs(
        argv, missingIndex, missingCount, llvm::opt::Visibility(options::ClangOption));
    if (missingCount) return "incomplete C++ audit driver option before frontend invocation";

    llvm::Triple target(llvm::Triple::normalize(
        args.getLastArgValue(options::OPT_target, llvm::sys::getDefaultTargetTriple())));
    // Darwin -arch changes the architecture only for Mach-O targets. Its
    // supported CPU spellings normalize to CPU architectures; unknown spellings
    // remain UnknownArch for Clang's own diagnostic rather than an AMD fallback.
    if (target.isOSBinFormatMachO()) {
        if (const auto* arch = args.getLastArg(options::OPT_arch)) {
            llvm::Triple selected(llvm::Triple::normalize(std::string(arch->getValue()) + "-apple-darwin"));
            target.setArch(selected.getArch());
        }
    }
    if (const auto* width = args.getLastArg(options::OPT_m32, options::OPT_m64)) {
        const auto selected = width->getOption().matches(options::OPT_m32)
            ? target.get32BitArchVariant() : target.get64BitArchVariant();
        if (selected.getArch() != llvm::Triple::UnknownArch) target.setArch(selected.getArch());
    }

#if LLVM_VERSION_MAJOR >= 23
    auto inputType = types::TY_INVALID;
    if (const auto* language = args.getLastArg(options::OPT_x)) {
        if (llvm::StringRef(language->getValue()) != "none")
            inputType = types::lookupTypeForTypeSpecifier(language->getValue());
    }
    if (!args.hasArg(options::OPT_x) || args.getLastArgValue(options::OPT_x) == "none") {
        auto extension = std::filesystem::path(unit.file).extension().string();
        if (!extension.empty()) extension.erase(extension.begin());
        inputType = types::lookupTypeForExtension(extension);
    }

#endif
    const auto cpu = args.getLastArgValue(options::OPT_mcpu_EQ);
    const auto arch = args.getLastArgValue(options::OPT_march_EQ);
    auto selected = toolChain(target);
#if LLVM_VERSION_MAJOR >= 23
    // HIP's unified AMDGPU device toolchain translates the original mcpu before the
    // bound device architecture replaces it. CUDA uses its bound march instead.
    if (types::isHIP(inputType) && cpu == "native")
        return "C++ audit rejects -mcpu=native for HIP before frontend invocation; select an explicit GPU architecture";
    // In 22 HIPAMDToolChain instead translates with the host toolchain, then
    // replaces mcpu with the bound GPU architecture without native discovery.
#endif
    if (selected == ToolChain::AMDGPU) {
        auto effective = cpu;
#if LLVM_VERSION_MAJOR >= 23
        // 22 requires mcpu; 23 maps legacy march only when mcpu is absent.
        if (!args.hasArg(options::OPT_mcpu_EQ)) effective = arch;
#endif
        if (effective == "native")
            return "C++ audit rejects native AMDGPU discovery before frontend invocation; select an explicit GPU architecture";
    }
    if (selected == ToolChain::NVPTX && arch == "native")
        return "C++ audit rejects -march=native for NVPTX before frontend invocation; select an explicit GPU architecture";
    return {};
}
}
