#ifndef TOPO_ADAPTIVE_H
#define TOPO_ADAPTIVE_H

// @stability provisional
// User-facing API in `topo::adaptive`: init/shutdown/stats + the
// `Config` thresholds. `force_specialize_bytes` is explicitly a
// test-side surface (its docstring says so) and may move under
// `detail::`. Underlying ABI is versioned at TOPO_ADAPTIVE_ABI_VERSION.

#include <cstddef>
#include <cstdint>
#include <string>

namespace topo::adaptive {

struct Config {
    uint32_t warmup_calls = 50;      // calls before monitoring starts
    uint32_t monitor_ms = 500;       // monitoring interval (milliseconds)
    double deviation_ratio = 1.5;    // trigger threshold (runtime/baseline deviation)
    uint32_t verify_calls = 30;      // calls after JIT to verify improvement
    double deopt_ratio = 0.9;        // deopt if JIT <= AOT * this ratio
    uint32_t max_versions = 3;       // max JIT recompilations per pipeline
    uint64_t min_trigger_ns = 10000; // minimum runtime ns to trigger first JIT (10µs)
};

struct Stats {
    uint32_t specializations;
    uint32_t deoptimizations;
    uint32_t active_jit;
};

/// Start the adaptive monitoring thread.
/// Safe to call multiple times; only the first call takes effect.
void init(const Config& cfg = {});

/// Stop the monitoring thread and release resources.
void shutdown();

/// Query current adaptive statistics.
Stats stats();

/// Force-specialize a pipeline by name (for testing).
void force_specialize(const std::string& name);

/// Force-specialize a pipeline using caller-provided bitcode, bypassing
/// the `.topo_ir` / `.tp_meta` section-reading path.  Used by the runtime
/// unit tests to exercise the dispatch-pointer atomic-store path without
/// going through topo-build.  Production code should keep using
/// force_specialize() unless you have a specific need to inject IR bytes.
void force_specialize_bytes(const std::string& name,
                            const void* irBytes, std::size_t irSize,
                            const std::string& metaJson = {});

} // namespace topo::adaptive

#endif // TOPO_ADAPTIVE_H
