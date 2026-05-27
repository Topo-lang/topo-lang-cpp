# C++ span-emitter fixture.
#
# The fixture is intentionally O0 / -g so that any unexpected runtime
# regression (a spurious span, a mis-ordered begin/end) shows up clearly
# in lldb. The link to topo-observe is via the cmake target, which
# carries the include path and platform link extras transparently.

add_executable(spans_demo_cpp fixtures/spans_demo.cpp)
target_compile_options(spans_demo_cpp PRIVATE -g -O0 -fno-omit-frame-pointer)
target_link_libraries(spans_demo_cpp PRIVATE topo-observe)
set_target_properties(spans_demo_cpp PROPERTIES EXCLUDE_FROM_ALL FALSE)

# deterministic pass-event emitter fixture. Links the
# reusable libtopo-pass-event wire (the one LifetimeArena/TopoParallel
# reuse) and emits a fixed AOT⇄JIT variant-switch sequence so the
# topo-profile `pass_events` routing is exercised reproducibly.
add_executable(pass_events_demo_cpp fixtures/pass_events_demo.cpp)
target_compile_options(pass_events_demo_cpp PRIVATE -g -O0 -fno-omit-frame-pointer)
target_link_libraries(pass_events_demo_cpp PRIVATE topo-pass-event)
set_target_properties(pass_events_demo_cpp PROPERTIES EXCLUDE_FROM_ALL FALSE)

# deterministic arena pass-event emitter fixture.
# Second producer (LifetimeArenaPass) on the same reusable wire; emits a
# fixed arena open/close sequence carrying the new optional `bytes`
# field so the topo-profile `pass_events` routing of the sized variant
# is exercised reproducibly.
add_executable(arena_events_demo_cpp fixtures/arena_events_demo.cpp)
target_compile_options(arena_events_demo_cpp PRIVATE -g -O0 -fno-omit-frame-pointer)
target_link_libraries(arena_events_demo_cpp PRIVATE topo-pass-event)
set_target_properties(arena_events_demo_cpp PROPERTIES EXCLUDE_FROM_ALL FALSE)

# deterministic parallel pass-event emitter fixture.
# Third producer (TopoParallelPass) on the same reusable wire; emits a
# fixed task spawn/join sequence using the 4-arg (no-`bytes`) form so the
# topo-profile `pass_events` routing of a third distinct pass name is
# exercised reproducibly alongside the AdaptiveDispatch/LifetimeArena
# producers.
add_executable(parallel_events_demo_cpp fixtures/parallel_events_demo.cpp)
target_compile_options(parallel_events_demo_cpp PRIVATE -g -O0 -fno-omit-frame-pointer)
target_link_libraries(parallel_events_demo_cpp PRIVATE topo-pass-event)
set_target_properties(parallel_events_demo_cpp PROPERTIES EXCLUDE_FROM_ALL FALSE)
