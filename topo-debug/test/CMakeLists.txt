# fixtures for the topo-debug-cpp adapter.
#
# Compiled at -O0 -g so the local variables survive optimization and the
# line table is intact. -fno-omit-frame-pointer keeps backtraces readable
# when something inside the adapter trips on a stop reason other than the
# breakpoint.
#
# The fixture targets live here (not in topo-lang-cpp/test/) because their
# only consumer is the adapter under this directory; co-locating keeps the
# breakpoint line numbers and the binaries in lock-step.

# tiny_matrix — int matrix[16][16] one-hot, exercises the original
# default-variable-name path (variable name == "matrix").
add_executable(tiny_matrix fixtures/tiny_matrix.cpp)
target_compile_options(tiny_matrix PRIVATE -g -O0 -fno-omit-frame-pointer)
target_compile_definitions(tiny_matrix PRIVATE TOPO_DEBUG_FIXTURE=1)
set_target_properties(tiny_matrix PROPERTIES EXCLUDE_FROM_ALL FALSE)

# tiny_vector — double vec[8] ramp, exercises the `--var`
# parameterisation (variable name != "matrix") and the f64 leaf dtype path.
add_executable(tiny_vector fixtures/tiny_vector.cpp)
target_compile_options(tiny_vector PRIVATE -g -O0 -fno-omit-frame-pointer)
target_compile_definitions(tiny_vector PRIVATE TOPO_DEBUG_FIXTURE=1)
set_target_properties(tiny_vector PROPERTIES EXCLUDE_FROM_ALL FALSE)

# tiny_multi — int a[4] + int b[4] in the same frame; exercises
# the `--var a,b` multi-variable adapter protocol so cross-variable
# arithmetic queries (`sum(a) + sum(b)`) drive a single adapter spawn.
add_executable(tiny_multi fixtures/tiny_multi.cpp)
target_compile_options(tiny_multi PRIVATE -g -O0 -fno-omit-frame-pointer)
target_compile_definitions(tiny_multi PRIVATE TOPO_DEBUG_FIXTURE=1)
set_target_properties(tiny_multi PROPERTIES EXCLUDE_FROM_ALL FALSE)

# tiny_struct — Particle p[4] of {float x,y,z}; exercises
# aggregate-leaf emission. Built at -O2 -g (matching the
# day-1 SBType smoke that confirmed field offsets survive optimization)
# so the test also doubles as a regression guard that DWARF struct field
# layout remains readable under realistic dev-mode build flags.
add_executable(tiny_struct fixtures/tiny_struct.cpp)
target_compile_options(tiny_struct PRIVATE -g -O2 -fno-omit-frame-pointer)
target_compile_definitions(tiny_struct PRIVATE TOPO_DEBUG_FIXTURE=1)
set_target_properties(tiny_struct PROPERTIES EXCLUDE_FROM_ALL FALSE)

# project-level fixture (full topo-build flow).
#
# Unlike tiny_matrix / tiny_vector (built directly by CMake with -g -O0),
# this fixture exercises the declaration-driven path end-to-end: `topo build`
# parses main.topo, emits main.topo-dbg.json (the view registry), and
# produces a debuggable binary via the standard dev-mode pipeline. The
# fixture is copied to the build tree at configure time so `topo build`
# writes its outputs there, not into the source checkout.
set(_PROJECT_SIMPLE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project_simple)
set(TOPO_DEBUG_CPP_PROJECT_SIMPLE_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/project_simple
    CACHE INTERNAL "Project-simple fixture build dir")
file(MAKE_DIRECTORY ${TOPO_DEBUG_CPP_PROJECT_SIMPLE_DIR})
configure_file(${_PROJECT_SIMPLE_SRC}/Topo.toml
    ${TOPO_DEBUG_CPP_PROJECT_SIMPLE_DIR}/Topo.toml COPYONLY)
configure_file(${_PROJECT_SIMPLE_SRC}/main.topo
    ${TOPO_DEBUG_CPP_PROJECT_SIMPLE_DIR}/main.topo COPYONLY)
configure_file(${_PROJECT_SIMPLE_SRC}/main.cpp
    ${TOPO_DEBUG_CPP_PROJECT_SIMPLE_DIR}/main.cpp COPYONLY)

# project-level multi-variable
# summary fixture. Two host arrays in scope; the summary template's
# placeholders span both, so resolving it exercises the multi-var
# adapter protocol + the single-spawn summary batching path.
set(_PROJECT_MULTI_SRC ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project_multi)
set(TOPO_DEBUG_CPP_PROJECT_MULTI_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/project_multi
    CACHE INTERNAL "Project-multi fixture build dir")
file(MAKE_DIRECTORY ${TOPO_DEBUG_CPP_PROJECT_MULTI_DIR})
configure_file(${_PROJECT_MULTI_SRC}/Topo.toml
    ${TOPO_DEBUG_CPP_PROJECT_MULTI_DIR}/Topo.toml COPYONLY)
configure_file(${_PROJECT_MULTI_SRC}/main.topo
    ${TOPO_DEBUG_CPP_PROJECT_MULTI_DIR}/main.topo COPYONLY)
configure_file(${_PROJECT_MULTI_SRC}/main.cpp
    ${TOPO_DEBUG_CPP_PROJECT_MULTI_DIR}/main.cpp COPYONLY)

# native-lldb formatter ("benefit path") fixture.
#
# project_formatter declares a `Mesh` debug entry whose summary template
# renders the logical declared view (`<Mesh: N verts, M tris>`). main.cpp
# stack-allocates a `Mesh` VALUE with two split column arrays — exactly the
# shape a SoA data-layout transform leaves behind — so plain
# `lldb -b -o 'frame variable mesh'` dumps the raw columns, while the
# formatter loaded via `.lldbinit` collapses them to the declared summary.
# Copied to the build tree like project_simple so `topo build` writes
# main + main.topo-dbg.json there, not into the source checkout.
set(_PROJECT_FORMATTER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project_formatter)
set(TOPO_DEBUG_CPP_PROJECT_FORMATTER_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/project_formatter
    CACHE INTERNAL "Project-formatter fixture build dir")
file(MAKE_DIRECTORY ${TOPO_DEBUG_CPP_PROJECT_FORMATTER_DIR})
configure_file(${_PROJECT_FORMATTER_SRC}/Topo.toml
    ${TOPO_DEBUG_CPP_PROJECT_FORMATTER_DIR}/Topo.toml COPYONLY)
configure_file(${_PROJECT_FORMATTER_SRC}/main.topo
    ${TOPO_DEBUG_CPP_PROJECT_FORMATTER_DIR}/main.topo COPYONLY)
configure_file(${_PROJECT_FORMATTER_SRC}/main.cpp
    ${TOPO_DEBUG_CPP_PROJECT_FORMATTER_DIR}/main.cpp COPYONLY)

# project_chart debug-view build-smoke fixture. Same topo-build flow as
# project_simple; the .topo source declares a `ChartData` debug entry
# (two sliced parallel views + a summary template) consumed by the e2e
# test `e2e.topo-debug-cpp.project_chart.build`.
set(_PROJECT_CHART_SRC ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project_chart)
set(TOPO_DEBUG_CPP_PROJECT_CHART_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/fixtures/project_chart
    CACHE INTERNAL "Project-chart fixture build dir")
file(MAKE_DIRECTORY ${TOPO_DEBUG_CPP_PROJECT_CHART_DIR})
configure_file(${_PROJECT_CHART_SRC}/Topo.toml
    ${TOPO_DEBUG_CPP_PROJECT_CHART_DIR}/Topo.toml COPYONLY)
configure_file(${_PROJECT_CHART_SRC}/main.topo
    ${TOPO_DEBUG_CPP_PROJECT_CHART_DIR}/main.topo COPYONLY)
configure_file(${_PROJECT_CHART_SRC}/main.cpp
    ${TOPO_DEBUG_CPP_PROJECT_CHART_DIR}/main.cpp COPYONLY)
