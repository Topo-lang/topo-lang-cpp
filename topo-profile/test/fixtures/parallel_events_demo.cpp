// Deterministic parallel pass-event emitter.
//
// This fixture exercises the *reusable pass-event wire* for the third
// producer, TopoParallelPass. It links libtopo-pass-event and calls
// topo_pass_event_emit() directly with the exact shape TopoParallelPass
// injects at the task spawn / join moments: the parallelized pipeline
// as `subject`, the spawn/join moment in from/to ("serial"->"spawned"
// on spawn, "spawned"->"joined" on join). The 4-arg (no-`bytes`) form
// is used — there is no natural numeric magnitude at a spawn/join
// moment, so TopoParallelPass deliberately reuses the simpler entry
// point (AdaptiveDispatch's form), leaving the optional `bytes` field
// absent.
//
// Why not drive the real TopoParallelPass here: that requires a full
// topo-build of a .topo project with [parallel].mode="force" plus the
// LLVM backend; the IR-pass injection itself is covered deterministically
// by the equivalence pipeline/forced firing guard (which now asserts
// >=1 spawn + >=1 join pass-event), and the runtime emitter shape is
// covered by the runtime unit tests. This e2e validates the wire-format
// + topo-profile `pass_events` routing contract — specifically that a
// TopoParallelPass spawn/join pair routes into a top-level
// `pass_events.TopoParallelPass[]` array and that the AdaptiveDispatch /
// LifetimeArena routing and the `spans` key stay unaffected — with a
// fixed, reproducible event sequence.
//
// Emits, in order:
//   pass_event TopoParallelPass serial->spawned  subject=stage_pipeline
//   pass_event TopoParallelPass spawned->joined  subject=stage_pipeline

#include <topo/rt/pass_event_rt.h>

#include <chrono>
#include <thread>

int main() {
    // Total wall >=15 ms keeps the child alive past PipedProcess's 5 ms
    // exec-failure detection window (same constraint spans_demo.cpp /
    // pass_events_demo.cpp / arena_events_demo.cpp document); a
    // faster-exiting child races the draining parent and is misread as a
    // spawn failure.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));

    // Spawn moment: a serial pipeline node becomes a set of tasks.
    topo_pass_event_emit("TopoParallelPass", "serial", "spawned",
                         "stage_pipeline");

    // Gap so the two records carry distinct, monotonically increasing
    // ts_ns values and so total wall stays past the 5 ms window.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));

    // Join moment: the spawned tasks are awaited back.
    topo_pass_event_emit("TopoParallelPass", "spawned", "joined",
                         "stage_pipeline");

    return 0;
}
