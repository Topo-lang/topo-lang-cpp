// Minimal C++ span-emitter for the topo-profile demo.
//
// Links libtopo-observe (the existing C ABI runtime — Topo does not
// reinvent profilers per the project philosophy), opens a tracing
// session, emits three named spans following the
// `pipeline::<name>::stage<N>` convention so the CLI can derive stage /
// pipeline metadata, and shuts down. No ObservabilityPass involvement;
// this fixture demonstrates that any binary linked against the runtime
// can be profiled, regardless of whether topo-build optimised it.

#include <topo/rt/observe_rt.h>
#include <chrono>
#include <cstdint>
#include <thread>

static void busy(int64_t us) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::microseconds(us);
    volatile uint64_t acc = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 1000; ++i) acc ^= static_cast<uint64_t>(i);
    }
    (void)acc;
}

int main() {
    topo_trace_init("stdout", 1.0);

    // Total wall ≥15 ms keeps the child alive past PipedProcess's 5 ms
    // exec-failure detection window; tighter spans would race the parent.
    topo_trace_span_begin("pipeline::demo::stage0");
    busy(5000);
    topo_trace_span_end();

    topo_trace_span_begin("pipeline::demo::stage1");
    busy(5000);
    topo_trace_span_end();

    topo_trace_span_begin("pipeline::demo::stage2");
    busy(5000);
    topo_trace_span_end();

    topo_trace_shutdown();
    return 0;
}
