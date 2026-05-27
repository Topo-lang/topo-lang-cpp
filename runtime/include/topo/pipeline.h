#ifndef TOPO_PIPELINE_H
#define TOPO_PIPELINE_H

// @stability stable
// Public user-facing surface is the TOPO_PIPELINE macro (and the
// downstream codegen that PipelineCodeGenPass performs). The
// `topo::detail::pipeline_placeholder` template + its annotation
// string are internal — they live under `detail::` and are exempt
// from the stable tier (PipelineCodeGenPass owns both sides).

// MSVC's cl.exe silently ignores GNU __attribute__ syntax, so the
// pipeline placeholder marker never reaches the LLVM IR and
// PipelineCodeGenPass produces zero codegen with no warning. There is
// no MSVC-compatible substitute (the toolchain doesn't emit LLVM IR
// the Topo passes can rewrite). Fail loudly instead of silently
// shipping broken pipelines.
#if defined(_MSC_VER) && !defined(__clang__)
#  error "topo/pipeline.h requires Clang's LLVM IR backend; MSVC's cl.exe cannot carry the __attribute__((annotate)) marker the Topo PipelineCodeGenPass relies on. Use Clang/LLVM (clang-cl on Windows works) for Topo-compiled translation units that use TOPO_PIPELINE."
#endif

#include <type_traits>

namespace topo {
namespace detail {

/// Placeholder function used by TOPO_PIPELINE macro.
/// The __attribute__((annotate(...))) causes Clang to emit an entry in
/// @llvm.global.annotations, allowing PipelineCodeGenPass to identify
/// pipeline stub functions.
///
/// Fallback detection: demangled name matching for
/// "topo::detail::pipeline_placeholder".
template <typename T>
__attribute__((annotate("topo_pipeline_placeholder"), noinline)) T pipeline_placeholder() {
    if constexpr (std::is_void_v<T>)
        return;
    else
        return T{};
}

} // namespace detail
} // namespace topo

/// Declare a pipeline stub function whose body will be replaced by
/// PipelineCodeGenPass with the auto-generated pipeline implementation.
///
/// Usage:
///   TOPO_PIPELINE(double, my_pipeline, (double x, int n))
///
/// Expands to a function definition that calls pipeline_placeholder<RetType>(),
/// which serves as a recognizable marker in the LLVM IR.
#define TOPO_PIPELINE(RetType, Name, Params)                    \
    __attribute__((optnone)) RetType Name Params {              \
        return ::topo::detail::pipeline_placeholder<RetType>(); \
    }

#endif // TOPO_PIPELINE_H
