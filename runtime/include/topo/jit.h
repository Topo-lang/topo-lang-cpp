#ifndef TOPO_JIT_H
#define TOPO_JIT_H

// @stability provisional
// User-facing API for JIT specialization is still settling: the
// `Context` builder surface and `narrow_returns`/`pin_args` shape
// may extend (additively) before declared stable. Underlying ABI
// is versioned at TOPO_JIT_ENGINE_ABI_VERSION (see jit_engine_rt.h)
// — that contract IS stable; only the C++ wrapper is provisional.

#include <cstddef>
#include <future>
#include <string>
#include <vector>

namespace topo::jit {

/// Runtime constraints for JIT specialization.
class Context {
public:
    /// Narrow the return fields actually used by downstream nodes.
    void narrow_returns(const std::string& func, const std::vector<std::string>& fields);

    /// Remove an edge from the pipeline DAG.
    void prune_edge(const std::string& src, const std::string& dst);

    /// Set a key-value runtime parameter.
    void set(const std::string& key, const std::string& value);

    // --- Accessors for the JIT engine ---
    struct PrunedEdge {
        std::string source;
        std::string target;
    };
    struct NarrowedReturn {
        std::string func;
        std::vector<std::string> fields;
    };

    const std::vector<PrunedEdge>& prunedEdges() const { return prunedEdges_; }
    const std::vector<NarrowedReturn>& narrowedReturns() const { return narrowedReturns_; }
    const std::vector<std::pair<std::string, std::string>>& params() const { return params_; }

private:
    std::vector<PrunedEdge> prunedEdges_;
    std::vector<NarrowedReturn> narrowedReturns_;
    std::vector<std::pair<std::string, std::string>> params_;
};

/// Asynchronously specialize a pipeline with runtime constraints.
/// Returns a future that resolves to the new function address (or nullptr on failure).
/// After completion, subsequent calls to the pipeline automatically use the
/// specialized version via atomic function pointer replacement.
std::future<void*> specialize(const std::string& pipelineName, const Context& ctx);

/// Check if JIT is available on this platform.
bool available();

/// Dump the specialized IR for debugging purposes.
std::string dump_ir(const std::string& pipelineName, const Context& ctx);

/// Asynchronously specialize a pipeline using caller-provided bitcode,
/// bypassing the `.topo_ir` / `.tp_meta` section reads.  Primary caller
/// is the runtime unit-test harness, which does not link through
/// topo-build and therefore has no embedded IR section.  Production code
/// should continue to use specialize() unless you have a specific need
/// to feed IR bytes directly.
///
/// @param pipelineName  Qualified name (post-demangle) to look up in the bitcode.
/// @param irBytes       LLVM bitcode buffer. The implementation COPIES this
///                      buffer before returning, so the caller may free it
///                      as soon as the call returns — the copy backs the
///                      async lambda for the full lifetime of the returned
///                      future. (Earlier revisions captured the raw pointer
///                      by value, which silently put the lifetime contract
///                      on the caller; that hazard is gone.)
/// @param irSize        Size of irBytes.
/// @param metaJson      Optional metadata JSON (may be empty when no pipeline
///                      edge/stage data applies). Captured by value.
/// @param ctx           Runtime constraints. Captured by value.
std::future<void*> specialize_bytes(const std::string& pipelineName,
                                    const void* irBytes, std::size_t irSize,
                                    const std::string& metaJson,
                                    const Context& ctx);

/// Dump caller-provided bitcode as a textual IR string.  Mirror of
/// dump_ir() for the bytes-injection path.
std::string dump_ir_bytes(const void* irBytes, std::size_t irSize);

} // namespace topo::jit

#endif // TOPO_JIT_H
