// Deterministic pass-event emitter for the topo-profile demo.
//
// This fixture exercises the reusable pass-event wire
// (the same wire LifetimeArenaPass / TopoParallelPass
// reuse). It links libtopo-pass-event and calls topo_pass_event_emit
// directly with the exact shape the adaptive runtime produces at a
// jitPtr flip — an AOT⇄JIT variant switch for a named pipeline.
//
// Why not drive the real adaptive monitor here: a true runtime switch
// goes through topo::jit::specialize(), which dynamically loads the JIT
// engine and depends on warm-up timing — non-deterministic and unfit
// for CTest. The IR-pass injection and the runtime emit sites are
// covered deterministically by AdaptiveDispatchPassTest.* (no JIT) and
// the runtime unit tests. This e2e validates the wire-format + the
// topo-profile `pass_events` routing contract that all three Passes
// share, with a fixed, reproducible event sequence.
//
// Emits, in order:
//   pass_event AdaptiveDispatchPass aot->jit subject=pipeline::demo
//   pass_event AdaptiveDispatchPass jit->aot subject=pipeline::demo
// plus one libtopo-observe-shaped span line is intentionally absent
// (a separate fixture covers spans); this binary's stdout is pass
// events only, proving topo-profile separates the record types by the
// "kind":"pass_event" discriminator rather than by stream position.

#include <topo/rt/pass_event_rt.h>

#include <chrono>
#include <thread>

int main() {
    // Total wall ≥15 ms keeps the child alive past PipedProcess's 5 ms
    // exec-failure detection window (same constraint spans_demo.cpp
    // documents); a faster-exiting child races the draining parent and
    // is misread as a spawn failure.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));

    // Two deterministic switches for the same pipeline. The runtime
    // would emit these from commitSpecialization (aot->jit) and a
    // deopt site (jit->aot); here we reproduce that exact wire.
    topo_pass_event_emit("AdaptiveDispatchPass", "aot", "jit",
                         "pipeline::demo");

    // Gap so the two records carry distinct, monotonically increasing
    // ts_ns values and so total wall stays past the 5 ms window.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));

    topo_pass_event_emit("AdaptiveDispatchPass", "jit", "aot",
                         "pipeline::demo");

    return 0;
}
