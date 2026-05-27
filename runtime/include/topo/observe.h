#ifndef TOPO_OBSERVE_H
#define TOPO_OBSERVE_H

// @stability stable
// Public user-facing API: `topo::observe::init/shutdown` C++ wrappers
// over the libtopo-observe C ABI. Underlying ABI is versioned at
// TOPO_OBSERVE_ABI_VERSION (see observe_rt.h).

#include "topo/rt/observe_rt.h"
#include <string>

namespace topo::observe {

/// Initialize the tracing runtime.
/// @param exporter  "stdout" (default) or "json"
/// @param rate      Sampling rate [0.0, 1.0]
inline void init(const std::string& exporter = "stdout", double rate = 1.0) {
    topo_trace_init(exporter.c_str(), rate);
}

/// Shut down the tracing runtime and flush output.
inline void shutdown() {
    topo_trace_shutdown();
}

} // namespace topo::observe

#endif // TOPO_OBSERVE_H
