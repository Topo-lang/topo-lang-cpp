#include "CppAuditDriver.h"
#include "../analysis/CppCompileModel.h"
#include <gtest/gtest.h>
#include <llvm/Config/llvm-config.h>
#include <vector>

using namespace topo::check;
namespace {
struct Case {
    const char* name;
    std::vector<std::string> arguments;
    bool rejected;
    std::string file = "/tmp/input.cpp";
};
class CppAuditDriverTest : public ::testing::TestWithParam<Case> {};
TEST_P(CppAuditDriverTest, ResolvesTheEffectiveArgumentsBeforeInvokingDriver) {
    CppCompileUnit unit;
    unit.arguments = GetParam().arguments;
    unit.file = GetParam().file;
    unit.compiler = "/installed/clang++";
    const auto reason = cppAuditDriverRejection(unit);
    EXPECT_EQ(!reason.empty(), GetParam().rejected) << reason;
    if (GetParam().rejected) EXPECT_NE(reason.find("before frontend invocation"), std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(GpuNativeBoundary, CppAuditDriverTest, ::testing::Values(
    Case{"AmdNative", {"--target=amdgcn-amd-amdhsa", "-mcpu=native"}, true},
    Case{"AmdExplicit", {"--target=amdgcn-amd-amdhsa", "-mcpu=gfx906"}, false},
    Case{"AmdLastExplicit", {"--target=amdgcn-amd-amdhsa", "-mcpu=native", "-mcpu=gfx906"}, false},
    Case{"AmdLastNative", {"--target=amdgcn-amd-amdhsa", "-mcpu=gfx906", "-mcpu=native"}, true},
    Case{"AmdCpuWinsOverMarch", {"--target=amdgcn-amd-amdhsa", "-mcpu=gfx906", "-march=native"}, false},
    Case{"AmdNativeCpuWinsOverMarch", {"--target=amdgcn-amd-amdhsa", "-mcpu=native", "-march=gfx906"}, true},
    Case{"AmdLegacyMarch", {"--target=amdgcn-amd-amdhsa", "-march=native"}, LLVM_VERSION_MAJOR >= 23},
    Case{"AmdLastExplicitMarch", {"--target=amdgcn-amd-amdhsa", "-march=native", "-march=gfx906"}, false},
    Case{"AmdNormalizedTarget", {"--target=amdgcn-amdhsa", "-mcpu=native"}, true},
    Case{"AmdPAL", {"--target=amdgcn-amd-amdpal", "-mcpu=native"}, true},
    Case{"AmdMesa", {"--target=amdgcn-mesa-mesa3d", "-mcpu=native"}, true},
    Case{"SpirvAMDHasDifferentToolChain", {"--target=spirv64-amd-amdhsa", "-mcpu=native"}, false},
    Case{"AmdUnknownOS", {"--target=amdgcn-unknown-unknown", "-mcpu=native"}, LLVM_VERSION_MAJOR >= 23},
    Case{"AmdLinuxHasDifferentToolChain", {"--target=amdgcn-unknown-linux", "-mcpu=native"}, false},
    Case{"AmdDarwinHasDifferentToolChain", {"--target=amdgcn-apple-darwin", "-mcpu=native"}, false},
    Case{"NvptxNative", {"--target=nvptx64-nvidia-cuda", "-march=native"}, true},
    Case{"NvptxExplicit", {"--target=nvptx64-nvidia-cuda", "-march=sm_52"}, false},
    Case{"NvptxLastExplicit", {"--target=nvptx64-nvidia-cuda", "-march=native", "-march=sm_52"}, false},
    Case{"NvptxLastNative", {"--target=nvptx64-nvidia-cuda", "-march=sm_52", "-march=native"}, true},
    Case{"NvptxMcpuNotDiscovery", {"--target=nvptx64-nvidia-cuda", "-mcpu=native", "-march=sm_52"}, false},
    Case{"Nvptx32Bit", {"--target=nvptx64-nvidia-cuda", "-m32", "-march=native"}, true},
    Case{"TargetHostOverridesGpu", {"--target=amdgcn-amd-amdhsa", "-target", "aarch64-apple-darwin", "-mcpu=native"}, false},
    Case{"TargetGpuOverridesHost", {"--target=aarch64-apple-darwin", "-target", "amdgcn-amd-amdhsa", "-mcpu=native"}, true},
    Case{"CpuNative", {"--target=x86_64-unknown-linux", "-march=native"}, false},
    Case{"CpuNativeMcpu", {"--target=aarch64-apple-darwin", "-mcpu=native"}, false},
    Case{"DefaultCpuNative", {"-march=native", "-mcpu=native"}, false},
    Case{"HipNative", {"--target=aarch64-apple-darwin", "-x", "hip", "-mcpu=native"}, LLVM_VERSION_MAJOR >= 23},
    Case{"HipLastExplicit", {"-x", "hip", "-mcpu=native", "-mcpu=gfx906"}, false},
    Case{"HipLastNative", {"-x", "hip", "-mcpu=gfx906", "-mcpu=native"}, LLVM_VERSION_MAJOR >= 23},
    Case{"HipMarchNotDiscovery", {"-x", "hip", "-march=native"}, false},
    Case{"HipExtension", {"-mcpu=native"}, LLVM_VERSION_MAJOR >= 23, "/tmp/input.hip"},
    Case{"HipPreprocessed", {"-x", "hip-cpp-output", "-mcpu=native"}, LLVM_VERSION_MAJOR >= 23},
    Case{"CppOverridesHip", {"-x", "hip", "-x", "c++", "-mcpu=native"}, false},
    Case{"NoneResolvesCppExtension", {"-x", "hip", "-x", "none", "-mcpu=native"}, false},
    Case{"NoneResolvesHipExtension", {"-x", "c++", "-x", "none", "-mcpu=native"}, LLVM_VERSION_MAJOR >= 23, "/tmp/input.hip"},
    Case{"CudaUsesBoundMarch", {"-x", "cuda", "-march=native"}, false},
    Case{"DefineValueIsNotTarget", {"-D", "--target=amdgcn-amd-amdhsa", "-mcpu=native"}, false},
    Case{"IncludeValueIsNotArchitecture", {"--target=amdgcn-amd-amdhsa", "-I", "-mcpu=native"}, false},
    Case{"DefineDoesNotClearNative", {"--target=amdgcn-amd-amdhsa", "-mcpu=native", "-D", "-mcpu=gfx906"}, true},
    Case{"DarwinArchClearsFallbackGpu", {"--target=amdgcn-unknown-unknown-macho", "-arch", "arm64", "-mcpu=native"}, false}
), [](const auto& param) { return param.param.name; });
}
