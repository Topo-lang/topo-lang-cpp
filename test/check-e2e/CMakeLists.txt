# Checker E2E suite (completeness + containment + import-path + purity +
# visibility + stage-isolation + showcase) — in its own file so the meta
# build can re-add it AFTER its topo-cli sibling configures (the inline
# add from ../CMakeLists.txt self-skips there: no topo::cli::TopoCheckRunner
# exists yet at lang-package configure time, and the meta puts no installed
# topo-cli on the prefix). Self-gating + idempotent: safe to
# add_subdirectory() twice — whichever pass first finds the runner defines
# the suite.
#
# Also includes fixture-check-clean.cmake (the per-fixture topo-check guard
# over the topo-debug project fixtures) — same late-hook rationale: its
# topo-check target only exists after topo-cli configures.

if(TARGET topo-lang-cpp-check-tests)
    return()
endif()

# TestMain.cpp must call topo::lang::registerCppPlugin() before tests run,
# so the suite cannot stand without the plugin. Defensive only —
# TopoCppPlugin builds unconditionally in every configure of this package.
if(NOT TARGET TopoCppPlugin)
    message(STATUS "topo-lang-cpp tests: checker-e2e suite (topo-lang-cpp-check-tests) "
                   "skipped — TopoCppPlugin not in scope.")
    return()
endif()

# The suite links TopoCheckRunner + its CheckRunner.h, defined by topo-cli
# (topo::cli::TopoCheckRunner) — a package DOWNSTREAM of topo-lang-cpp (a
# hard dependency would be a circular edge). Two paths in:
#   * meta build — topo-cli's in-tree alias satisfies the gate on the meta
#     root's late re-add of this dir;
#   * standalone — an installed topo-cli on CMAKE_PREFIX_PATH exports the
#     runner. This is what the verify-llvm CI job uses, so cpp L2
#     containment actually RUNS on a public surface instead of being
#     meta-only.
if(NOT TARGET topo::cli::TopoCheckRunner)
    find_package(topo-cli CONFIG QUIET)
endif()
if(NOT TARGET topo::cli::TopoCheckRunner)
    message(STATUS "topo-lang-cpp tests: checker-e2e suite (topo-lang-cpp-check-tests) "
                   "skipped — needs TopoCheckRunner (in-tree via the meta build, or an "
                   "installed topo-cli on CMAKE_PREFIX_PATH).")
    return()
endif()

# Sources and fixtures stay in test/e2e + test/fixtures; anchor on this
# file's parents so both the inline and the meta-late add resolve them.
get_filename_component(_test_root "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
get_filename_component(_pkg_root "${_test_root}" DIRECTORY)

add_executable(topo-lang-cpp-check-tests
    ${_test_root}/e2e/TestMain.cpp
    ${_test_root}/e2e/CppCompletenessTest.cpp
    ${_test_root}/e2e/CppContainmentTest.cpp
    ${_test_root}/e2e/CppImportPathTest.cpp
    ${_test_root}/e2e/CppPurityTest.cpp
    ${_test_root}/e2e/CppShowcaseTest.cpp
    ${_test_root}/e2e/CppVisibilityTest.cpp
    ${_test_root}/e2e/CppStageIsolationTest.cpp
)
# TopoCppLSP is unconditional and provides ClangdBridge for CppContainmentTest.
target_link_libraries(topo-lang-cpp-check-tests PRIVATE
    topo::cli::TopoCheckRunner TopoCppPlugin TopoCppLSP
    GTest::gtest
)
target_compile_definitions(topo-lang-cpp-check-tests PRIVATE
    TOPO_TEST_FIXTURES_DIR="${_test_root}/fixtures/check"
    TOPO_EXAMPLES_DIR="${_pkg_root}/examples"
)
topo_set_compiler_flags(topo-lang-cpp-check-tests)
gtest_discover_tests(topo-lang-cpp-check-tests DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "toolchain\;checker\;lang-cpp" TIMEOUT 60)

# Per-fixture check-clean ctest guard over the topo-debug project fixtures
# (rc=0 AND warning-free stdout). Self-gates on the in-tree topo-check
# target; see fixture-check-clean.cmake for the full rationale.
include("${CMAKE_CURRENT_LIST_DIR}/fixture-check-clean.cmake")
