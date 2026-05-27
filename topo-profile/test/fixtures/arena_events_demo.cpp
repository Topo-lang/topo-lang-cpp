// Deterministic arena pass-event emitter.
//
// This fixture exercises the *reusable pass-event wire* for the second
// producer, LifetimeArenaPass. It links libtopo-pass-event and calls
// topo_pass_event_emit_sized() directly with the exact shape
// LifetimeArenaPass injects at arena open/close sites: the lifetime
// scope as `subject`, the open/close moment in from/to ("heap"->"arena"
// on open, "arena"->"freed" on close), and the arena size in the new
// optional `bytes` field.
//
// Why not drive the real LifetimeArenaPass here: that requires a full
// topo-build of a .topo project with [lifetime].mode="force" plus the
// LLVM backend; the IR-pass injection itself is covered deterministically
// by the LifetimeArenaPass GTest unit tests (no backend build needed),
// and the runtime emitter shape is covered by the runtime unit tests.
// This e2e validates the wire-format + topo-profile `pass_events`
// routing contract — specifically that the new optional `bytes` field
// round-trips and that AdaptiveDispatch events (no `bytes`) are
// unaffected — with a fixed, reproducible event sequence.
//
// Emits, in order:
//   pass_event LifetimeArenaPass heap->arena  subject=request_scope bytes=4096
//   pass_event LifetimeArenaPass arena->freed subject=request_scope bytes=1280

#include <topo/rt/pass_event_rt.h>

#include <chrono>
#include <thread>

int main() {
    // Total wall >=15 ms keeps the child alive past PipedProcess's 5 ms
    // exec-failure detection window (same constraint spans_demo.cpp /
    // pass_events_demo.cpp document); a faster-exiting child races the
    // draining parent and is misread as a spawn failure.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));

    // Arena open: requested capacity is the size payload.
    topo_pass_event_emit_sized("LifetimeArenaPass", "heap", "arena",
                               "request_scope", 4096);

    // Gap so the two records carry distinct, monotonically increasing
    // ts_ns values and so total wall stays past the 5 ms window.
    std::this_thread::sleep_for(std::chrono::milliseconds(8));

    // Arena close: bytes actually used is the size payload.
    topo_pass_event_emit_sized("LifetimeArenaPass", "arena", "freed",
                               "request_scope", 1280);

    return 0;
}
