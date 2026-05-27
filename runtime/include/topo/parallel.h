#ifndef TOPO_PARALLEL_H
#define TOPO_PARALLEL_H

// @stability stable
// Public user-facing API in `topo::parallel`: init/shutdown +
// cost-sample query/reset, with a stable `Config` shape. Internal
// helpers belong in the libtopo-parallel C ABI (parallel_rt.h) and
// are versioned by TOPO_PARALLEL_ABI_VERSION; this header is the
// idiomatic C++ wrapper over that ABI.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace topo::parallel {

struct Config {
    int num_threads = 0;    // 0 = hardware_concurrency
    bool instrument = true; // enable cost sampling (low overhead)
};

/// Initialize the parallel runtime (thread pool + sampling).
/// Safe to call multiple times; only the first call takes effect.
void init(const Config& cfg = {});

/// Shut down the thread pool and release resources.
void shutdown();

/// Query aggregated cost samples: function name -> average nanoseconds.
std::unordered_map<std::string, uint64_t> get_cost_samples();

/// Reset all accumulated cost samples.
void reset_cost_samples();

} // namespace topo::parallel

#endif // TOPO_PARALLEL_H
